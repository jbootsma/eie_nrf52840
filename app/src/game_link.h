#ifndef GAME_LINK_H
#define GAME_LINK_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/net_buf.h>

#define NAME_MAX_LEN 10
#define MSG_MAX_LEN 18

typedef struct {
  char name[NAME_MAX_LEN];
} player_name_t;

typedef enum {
  LINK_EVT_DISCOVERY,
  LINK_EVT_JOIN,
  LINK_EVT_JOIN_ERR,
  LINK_EVT_PEER_JOIN,
  LINK_EVT_DISCONNECT,
  LINK_EVT_MSG,
  LINK_EVT_GAME_START,
} link_evt_e;

typedef struct {
  link_evt_e code;

  union {
    struct {
      bt_addr_le_t addr;
      player_name_t host_name;
      uint8_t player_count;
    } discovery;

    struct {
      uint8_t player_id;
    } join;

    struct {
      uint8_t player_id;
      player_name_t player_name;
    } peer_join;

    struct {
      uint8_t player_id;
    } disconnect;

    struct {
      uint8_t from_player_id;
      // Not nescessarily efficient, but simple.
      // No worries about lifetime of the data.
      uint8_t msg[MSG_MAX_LEN];
      size_t len;
    } msg;
  } params;
} link_evt_t;

typedef void (*game_link_cb)(const link_evt_t *evt);

void game_link_init(game_link_cb cb);
void game_link_proc_evts(void);

void game_link_set_name(const player_name_t *name);

void game_link_discover(void);
void game_link_host(void);
void game_link_join(bt_addr_le_t tgt);
void game_link_stop(void);

void game_link_bcast_msg(const uint8_t *data, size_t len);
void game_link_start_game(void);

#endif
