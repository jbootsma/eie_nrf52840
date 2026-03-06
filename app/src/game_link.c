#include <stdlib.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "game_link.h"

LOG_MODULE_REGISTER(game_link, LOG_LEVEL_DBG);

#define HOST_IDX 0

//---- BLE ---- //

typedef enum {
  INT_EVT_SCAN_RES,
  INT_EVT_CONNECT,
  INT_EVT_DISCONNECT,
  INT_EVT_HANDLE_DISC,
  INT_EVT_MSG,
  INT_EVT_FATAL_ERR,
} internal_evt_e;

typedef struct {
  player_name_t host_name;
  uint8_t players;
} adv_service_data_t;

typedef struct {
  void *fifo_reserved;
  internal_evt_e code;
  union {
    struct {
      bt_addr_le_t addr;
      adv_service_data_t serv_data;
    } scan_res;

    struct {
      struct bt_conn *conn;
      uint8_t err;
    } connect;

    struct {
      struct bt_conn *conn;
      uint8_t reason;
    } disconnect;

    struct {
      struct bt_conn *conn;
      uint16_t handle;
    } handle_disc;

    struct {
      struct bt_conn *src;
      size_t len;
      uint8_t msg[MSG_MAX_LEN];
    } msg;

    struct {
      struct bt_conn *conn;
    } fatal_err;
  } params;
} internal_evt_t;

static bool process_advertising_field(struct bt_data *data, void *user_data);
static uint8_t service_discovery_cb(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    struct bt_gatt_discover_params *params);

static void on_scan_result(const bt_addr_le_t *addr, int8_t rssi,
                           uint8_t adv_type, struct net_buf_simple *buf);
static void on_connected(struct bt_conn *conn, uint8_t err);
static void on_disconnected(struct bt_conn *conn, uint8_t reason);
static ssize_t on_msg_received(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr, const void *buf,
                               uint16_t len, uint16_t offset, uint8_t flags);

static void send_msg(size_t idx, const void *data, size_t len);
static void send_client_hello(void);

BT_CONN_CB_DEFINE(conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};

// Follow the example fo the BT sig, use a 96 bit base UUID and derive our UUIDs
// from that.
#define GL_UUID_VAL(val)                                                       \
  BT_UUID_128_ENCODE(0x00000000, 0x4873, 0x4a13, 0xb51b, 0x94b8fcc1d08d)

#define ADV_UUID_VAL GL_UUID_VAL(0)
static const struct bt_uuid_128 adv_uuid = BT_UUID_INIT_128(ADV_UUID_VAL);

static const struct bt_uuid_128 serv_uuid = BT_UUID_INIT_128(GL_UUID_VAL(1));
static const struct bt_uuid_128 msg_recv_uuid =
    BT_UUID_INIT_128(GL_UUID_VAL(2));

BT_GATT_SERVICE_DEFINE(
    game_link_service, BT_GATT_PRIMARY_SERVICE(&serv_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&msg_recv_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, on_msg_received, NULL));

static struct {
  struct bt_conn *conn;
  uint16_t handle;
} conns[CONFIG_BT_MAX_CONN];

//---- Messages ---- //

typedef enum {
  // Client -> Host on connection.
  MSG_CLIENT_HELLO,
  // Host -> Client after hello.
  MSG_SERVER_HELLO,
  // Host -> All clients after join. New players also get one for every existing
  // player.
  MSG_PLAYER_ANOUNCE,
  // Host -> All clients after disconnect.
  MSG_PLAYER_GONE,
  // Client -> Host -> All Clients. All messages are always broadcast to
  // everyone.
  MSG_GAME_MSG,
} msg_id_e;

typedef struct {
  uint8_t id;
  player_name_t name;
} client_hello_msg_t;

typedef struct {
  uint8_t id;
  uint8_t assigned_slot;
} server_hello_msg_t;

typedef struct {
  uint8_t id;
  uint8_t slot;
  player_name_t name;
} player_announce_msg_t;

typedef struct {
  uint8_t id;
  uint8_t slot;
} player_gone_msg_t;

typedef struct {
  uint8_t id;
  uint8_t reserved; // May be used in the future to send to specific players.
                    // Set to 0xFF.
  uint8_t data[MSG_MAX_LEN];
} game_msg_t;

//---- State Machine ---- //

typedef enum {
  LINK_OFF,
  LINK_SEARCHING,
  LINK_LOBBY,
  LINK_GAME,
} link_state_e;

typedef struct {
  void *fifo_reserved;
  link_evt_e evt;
} evt_q_item_t;

static void send_fatal_err(struct bt_conn *conn);
static void setup_msg_handle(size_t conn_idx);
static void assign_conn(struct bt_conn *conn);
static void kill_conn(struct bt_conn *conn);
static void disconnect(size_t conn_idx);
static bool handle_msg(size_t src, const void *msg, size_t len);

static game_link_cb link_cb;
static player_name_t player_name;

static bool is_host;
static link_state_e link_state;

static K_FIFO_DEFINE(internal_evts);

void game_link_init(game_link_cb cb) {
  link_cb = cb;

  if (0 != bt_enable(NULL)) {
    LOG_ERR("Could not initialize BT");
    return;
  }

  LOG_INF("BT ready");
}

void game_link_proc_evts(void) {
  internal_evt_t *evt;

  while (true) {
    evt = k_fifo_get(&internal_evts, K_NO_WAIT);
    if (evt == NULL) {
      break;
    }

    LOG_DBG("Internal evt %d", evt->code);

    switch (evt->code) {
    case INT_EVT_SCAN_RES: {
      link_evt_t out_evt = {
          .code = LINK_EVT_DISCOVERY,
          .params.discovery =
              {
                  .addr = evt->params.scan_res.addr,
                  .host_name = evt->params.scan_res.serv_data.host_name,
                  .player_count = evt->params.scan_res.serv_data.players,
              },
      };

      if (link_cb) {
        link_cb(&out_evt);
      }
    } break;

    case INT_EVT_CONNECT: {
      if (evt->params.connect.err) {
        LOG_ERR("Failed to connect: %u", evt->params.connect.err);
        kill_conn(evt->params.connect.conn);
      } else {
        if (is_host) {
          LOG_DBG("New connection");
          assign_conn(evt->params.connect.conn);
          // TODO: Restart advertising. Handle link state below as well?
        } else if (evt->params.connect.conn == conns[0].conn) {
          LOG_DBG("Connection complete");
          setup_msg_handle(0);
        }
      }

      bt_conn_unref(evt->params.connect.conn);
    } break;

    case INT_EVT_DISCONNECT: {
      LOG_DBG("Disconnect, reason=%u", evt->params.disconnect.reason);
      kill_conn(evt->params.connect.conn);
      bt_conn_unref(evt->params.disconnect.conn);
    } break;

    case INT_EVT_HANDLE_DISC: {
      bool found = false;

      for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
        if (conns[i].conn == evt->params.handle_disc.conn) {
          conns[i].handle = evt->params.handle_disc.handle;
          LOG_DBG("Found message handle for conn %zu: %u", i, conns[i].handle);

          if (!is_host) {
            send_client_hello();
          }

          found = true;
          break;
        }
      }

      if (!found) {
        LOG_WRN("Ignoring handle discovery");
      }

      bt_conn_unref(evt->params.handle_disc.conn);
    } break;

    case INT_EVT_MSG: {
      bool handled = true;

      for (size_t idx = 0; idx < ARRAY_SIZE(conns); idx++) {
        if (evt->params.msg.src == conns[idx].conn) {
          if (!handle_msg(idx, evt->params.msg.msg, evt->params.msg.len)) {
            LOG_ERR("Message handler err, disconnecting");
            disconnect(idx);
          }

          handled = true;
          break;
        }
      }

      if (!handled) {
        LOG_WRN("Message from untracked connection");
      }

      bt_conn_unref(evt->params.msg.src);
    } break;

    case INT_EVT_FATAL_ERR: {
      LOG_ERR("Fatal err, killing connection");
      kill_conn(evt->params.fatal_err.conn);
      bt_conn_unref(evt->params.fatal_err.conn);
    }
    }

    free(evt);
  }
}

void game_link_set_name(const player_name_t *name) {
  if (link_state == LINK_OFF) {
    LOG_DBG("Player name update");
    player_name = *name;
  }
}

void game_link_discover(void) {
  game_link_stop();

  static const struct bt_le_scan_param params = {
      .type = BT_LE_SCAN_TYPE_ACTIVE,
      .interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
      .window = BT_GAP_SCAN_FAST_WINDOW,
      .options = BT_LE_SCAN_OPT_NONE,
  };

  if (0 != bt_le_scan_start(&params, on_scan_result)) {
    LOG_ERR("Failed to start scan");
    return;
  }

  LOG_INF("Link discovery active");
  is_host = false;
  link_state = LINK_SEARCHING;
}

void game_link_host(void) {
  game_link_stop();

  struct bt_data adv_data[] = {
      BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
              sizeof(CONFIG_BT_DEVICE_NAME) - 1),
  };

  // The bumble simulator doesn't properly handle scan response data, so put the
  // parsed data in the primary adv response.
  struct {
    uint8_t uuid[128 / 8];
    adv_service_data_t serv_data;
  } serv_data = {
      .uuid = {ADV_UUID_VAL},
      .serv_data =
          {
              .host_name = player_name,
              .players = 1,
          },
  };

  struct bt_data scan_data[] = {{
      .type = BT_DATA_SVC_DATA128,
      .data = (uint8_t *)&serv_data,
      .data_len = sizeof(serv_data),
  }};

  static const struct bt_le_adv_param params = {
      .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY |
                 BT_LE_ADV_OPT_SCANNABLE,
      .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
      .interval_max = BT_GAP_ADV_FAST_INT_MAX_2};

  if (0 != bt_le_adv_start(&params, adv_data, ARRAY_SIZE(adv_data), scan_data,
                           ARRAY_SIZE(scan_data))) {
    LOG_ERR("Failed to start advertising");
    return;
  }

  LOG_INF("Hosting lobby");
  is_host = true;
  link_state = LINK_LOBBY;
}

void game_link_join(bt_addr_le_t tgt) {
  game_link_stop();

  static const struct bt_conn_le_create_param create_params = {
      .interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
      .window = BT_GAP_SCAN_FAST_WINDOW,
  };
  static const struct bt_le_conn_param conn_params = {
      .interval_min = BT_GAP_MS_TO_CONN_INTERVAL(100),
      .interval_max = BT_GAP_MS_TO_CONN_INTERVAL(100),
      .latency = 0,
      .timeout = BT_GAP_MS_TO_CONN_TIMEOUT(20000),
  };

  int err =
      bt_conn_le_create(&tgt, &create_params, &conn_params, &conns[0].conn);
  if (err) {
    LOG_ERR("Failed to create connection: %d", err);
    return;
  }

  is_host = false;
  link_state = LINK_LOBBY;
}

void game_link_stop(void) {
  LOG_INF("Stopping existing links");

  switch (link_state) {
  case LINK_SEARCHING:
    bt_le_scan_stop();
    break;

  case LINK_LOBBY:
  case LINK_GAME:
    bt_le_adv_stop();

    for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
      disconnect(i);
    }
    break;

  case LINK_OFF:
    break;
  }

  link_state = LINK_OFF;
}

void game_link_bcast_msg(const uint8_t *data, size_t len) {
  // TODO
}

void game_link_start_game(void) {
  // TODO
}

static bool process_advertising_field(struct bt_data *data, void *user_data) {
  LOG_DBG("Fty: %u", data->type);
  if (data->type != BT_DATA_SVC_DATA128) {
    return true;
  }

  if (data->data_len < (128 / 8) + sizeof(adv_service_data_t)) {
    LOG_WRN("Invalid service data len");
    return false;
  }

  if (0 != memcmp(data->data, adv_uuid.val, sizeof(adv_uuid.val))) {
    LOG_DBG("Other service");
    return true;
  }

  LOG_DBG("Has room data");

  internal_evt_t *evt = malloc(sizeof(*evt));
  evt->code = INT_EVT_SCAN_RES;
  evt->params.scan_res.addr = *(const bt_addr_le_t *)user_data;
  memcpy(&evt->params.scan_res.serv_data, data->data + sizeof(adv_uuid.val),
         sizeof(adv_service_data_t));

  LOG_DBG("Dispatch scan res");
  k_fifo_put(&internal_evts, evt);
  return false;
}

static uint8_t service_discovery_cb(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    struct bt_gatt_discover_params *params) {
  if (attr == NULL) {
    LOG_ERR("Discovery failed");
    send_fatal_err(conn);
  } else if (params->type == BT_GATT_DISCOVER_PRIMARY) {
    LOG_DBG("Service discovered");
    struct bt_gatt_service_val *val = attr->user_data;
    params->end_handle = val->end_handle;

    params->uuid = &msg_recv_uuid.uuid;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, params);

    if (err) {
      LOG_ERR("Failed to start characteristic discovery: %d", err);
      send_fatal_err(conn);
    } else {
      // Don't free the re-used params.
      params = NULL;
    }
  } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
    LOG_DBG("Characteristic discovered");
    struct bt_gatt_chrc *val = attr->user_data;

    internal_evt_t *evt = malloc(sizeof(*evt));
    evt->code = INT_EVT_HANDLE_DISC;
    evt->params.handle_disc.conn = bt_conn_ref(conn);
    evt->params.handle_disc.handle = val->value_handle;
    k_fifo_put(&internal_evts, evt);
  }

  free(params);
  return BT_GATT_ITER_STOP;
}

static void on_scan_result(const bt_addr_le_t *addr, int8_t rssi,
                           uint8_t adv_type, struct net_buf_simple *buf) {
  LOG_DBG("Adv report");

  bt_data_parse(buf, process_advertising_field, (void *)addr);
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
  internal_evt_t *evt = malloc(sizeof(*evt));
  evt->code = INT_EVT_CONNECT;
  evt->params.connect.conn = bt_conn_ref(conn);
  evt->params.connect.err = err;
  k_fifo_put(&internal_evts, evt);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
  internal_evt_t *evt = malloc(sizeof(*evt));
  evt->code = INT_EVT_DISCONNECT;
  evt->params.disconnect.conn = bt_conn_ref(conn);
  evt->params.disconnect.reason = reason;
  k_fifo_put(&internal_evts, evt);
}

static ssize_t on_msg_received(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr, const void *buf,
                               uint16_t len, uint16_t offset, uint8_t flags) {
  LOG_DBG("Got message");

  if (offset != 0 || len == 0) {
    return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
  }

  if (len > MSG_MAX_LEN) {
    return BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE);
  }

  internal_evt_t *evt = malloc(sizeof(*evt));
  evt->code = INT_EVT_MSG;
  evt->params.msg.src = bt_conn_ref(conn);
  evt->params.msg.len = len;
  memcpy(evt->params.msg.msg, buf, len);

  k_fifo_put(&internal_evts, evt);

  return len;
}

static void send_msg(size_t idx, const void *msg, size_t msg_len) {
  if (conns[idx].conn == NULL || conns[idx].handle == 0) {
    LOG_WRN("Message dropped due to missing connection");
    return;
  }

  LOG_DBG("Queue msg %u for %zu", *(const uint8_t *)msg, idx);

  // TODO: Use local message queueing. Or just make sure there's enough buffers.
  int err = bt_gatt_write_without_response(conns[idx].conn, conns[idx].handle,
                                           msg, msg_len, false);
  if (err) {
    LOG_ERR("Failed to send message to slot %zu, err=%d", idx, err);
    disconnect(idx);
  }
}

static void send_client_hello(void) {
  client_hello_msg_t msg = {
      .id = MSG_CLIENT_HELLO,
      .name = player_name,
  };

  LOG_DBG("Saying hello");
  send_msg(HOST_IDX, &msg, sizeof(msg));
}

static void send_fatal_err(struct bt_conn *conn) {
  internal_evt_t *evt = malloc(sizeof(*evt));
  evt->code = INT_EVT_FATAL_ERR;
  evt->params.fatal_err.conn = bt_conn_ref(conn);
  k_fifo_put(&internal_evts, evt);
}

static void setup_msg_handle(size_t conn_idx) {
  if (conns[conn_idx].handle != 0) {
    LOG_ERR("Discovery already completed!");
    return;
  }

  LOG_DBG("Starting handle discovery for %zu", conn_idx);

  struct bt_gatt_discover_params *params = calloc(1, sizeof(*params));
  params->uuid = &serv_uuid.uuid;
  params->func = service_discovery_cb;
  params->start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  params->end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  params->type = BT_GATT_DISCOVER_PRIMARY;

  int err = bt_gatt_discover(conns[conn_idx].conn, params);

  if (err) {
    LOG_ERR("Failed to start service discovery: %d", err);
    free(params);
    disconnect(conn_idx);
  }
}

static void assign_conn(struct bt_conn *conn) {
  for (size_t i = 1; i < ARRAY_SIZE(conns); i++) {
    if (conns[i].conn == NULL) {
      LOG_DBG("Assigning new connection to slot %zu", i);

      conns[i].conn = bt_conn_ref(conn);
      setup_msg_handle(i);

      return;
    }
  }
}

static void kill_conn(struct bt_conn *conn) {
  for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
    if (conns[i].conn == conn) {
      disconnect(i);
      return;
    }
  }
}

static void disconnect(size_t conn_idx) {
  if (conns[conn_idx].conn == NULL) {
    return;
  }

  LOG_DBG("Disconnecting %zu", conn_idx);

  bt_conn_disconnect(conns[conn_idx].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
  bt_conn_unref(conns[conn_idx].conn);
  memset(&conns[conn_idx], 0, sizeof(conns[conn_idx]));
}

static bool handle_msg(size_t src, const void *msg, size_t len) {
  uint8_t id = *(const uint8_t *)msg;

  LOG_DBG("Process msg %d from %zu", id, src);

  switch (id) {
  case MSG_CLIENT_HELLO: {
    if (!is_host) {
      LOG_ERR("Got hello as client");
      return false;
    }

    if (len < sizeof(client_hello_msg_t)) {
      LOG_ERR("Truncated hello");
      return false;
    }

    const client_hello_msg_t *hello = msg;

    // TODO: Announce
    char name[NAME_MAX_LEN + 1];
    memcpy(name, &hello->name, sizeof(hello->name));
    name[NAME_MAX_LEN] = 0;
    LOG_INF("Hello from %s", name);
    return true;
  }

  default:
    LOG_WRN("Unhandled msg %d", id);
    return false;
  }

  return false;
}
