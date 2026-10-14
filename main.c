/* 
 *  2026-10-07 03:21 -BwopBwop
 *
 * The reason this exist is because SDL doesn't expose the conversion from platform to sdl keycodes
 * and I figured if I was going to go through the effort of writing a conversion layer for x11.
 * that I might as well just do an application that just does the entirety of the linux platform.
 *
 * With the caveat that now this requires elevated permissions. However if someone is tech savy
 * enough to build this then they most likely can read this code in fact they might be reading this
 * block of text right now.
 * 
 * What this daemon will do is listen to events from /dev/input/ and send them to a Unix Domain Socket
 * of which another application can listen on. The daemon will do the keycode conversion to SDL automatically.
 * However later that could be toggled with a flag, who knows.
 */

#define _GNU_SOURCE 1

#include <x86intrin.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/time.h>

#define SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH "SDL_MOUSE_FOCUS_CLICKTHROUGH"

#define KEY_RELEASED 0
#define KEY_PRESSED 1
#define KEY_AUTOREPEAT 2

#define NUM_OF_MODIFIERS 11

#define SDL_TYPE_KEY_DOWN 0x300
#define SDL_TYPE_KEY_UP 0x301

#define Kilobytes(number) ((number) * 1024ull)
#define Megabytes(number) (Kilobytes(number) * 1024ull)
#define Gigabytes(number) (Megabytes(number) * 1024ull)

enum modifier_position {
 LCTRL,
 LALT,
 LSHIFT,
 RCTRL,
 RALT,
 RSHIFT,
 LGUI,
 RGUI,
 CAPS,
 SCROLL,
 NUM,
};

struct modifier_storage {
 int32_t modifiers[NUM_OF_MODIFIERS];
};

const char *path = "/run/user/1000/keyboardListener";

typedef struct memoryArena {
 uint32_t size;
 uint32_t used;

 void *memory;
} memoryArena;

#define MAX_CLIENTS 64
#define MAX_EVENTS 64
#define MAX_READ_EVENTS 64

typedef struct client {
 struct pollfd pfd;

 struct client *next;
 struct client *prev;
} client;
typedef struct client_list {
 client *head;
} client_list;

typedef struct event {
 int32_t type;
 int32_t scancode;
 uint16_t modstate;
} event;

typedef struct tui_state {
 int64_t tui_cycle_count;
 int64_t read_cycle_count;
 int32_t read_amount;
 int64_t send_cycle_count;
 int32_t send_amount;
 int32_t last_send_amount;
 int64_t process_cycle_count;
} tui_state;

int32_t convertToSDLScanCode(uint16_t scancode);
int16_t convertModiferState(struct modifier_storage * ms);
int32_t newClient(memoryArena *arena, client_list *list, client_list *free_list, int32_t fd, int16_t events);

void draw_tui(struct modifier_storage *ms, struct input_event *ev, client_list *list, tui_state* state);
int32_t handle_modifiers(uint16_t value, uint16_t code, struct modifier_storage *ms);

void *pushSize(memoryArena *arena, size_t size);

int32_t main() {

 remove(path);
 
 struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };
 strcpy(sockaddr.sun_path, path);

 int32_t socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
 if (socket_fd == -1) {
  printf("Unable to open socket\n");
  return(-1);
 }
 
 int32_t bind_result = bind(socket_fd, (struct sockaddr *)&sockaddr, offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1);
 if (bind_result == -1) {
  printf("Couldn't assign a name to a socket\n");
  close(socket_fd);
  return(-1);
 }
 
 int32_t listen_result = listen(socket_fd, 1);
 if (listen_result == -1) {
  printf("Socket was unable to listen to anyone\n");
  close(socket_fd);
  unlink(path);
  return(-1);
 }

 if (chmod(path, 0777) == -1) {
  perror("chmod");
  close(socket_fd);
  unlink(path);
  return 1;
 }

 {
  int32_t setflag_result = fcntl(socket_fd, F_SETFL, O_NONBLOCK);
  if (setflag_result == -1) {
   printf("Couldn't set socket to non blocking\n");
   close(socket_fd);
   unlink(path);
   return(-1);
  } 
 }

 // TODO: Find keyboard from ID's
 int32_t fd = open("/dev/input/event3", O_RDONLY);
 if (fd == -1) {
  printf("Unable to access keyboard\n");
  close(socket_fd);
  unlink(path);
  return(-1);
 }

 {
  int32_t setflag_result = fcntl(fd, F_SETFL, O_NONBLOCK);
  if (setflag_result == -1) {
   printf("Couldn't set socket to non blocking\n");
   close(socket_fd);
   close(fd);
   unlink(path);
   return(-1);
  } 
 }
 printf("File Descriptor: %d \n", fd);

 int32_t listener_fd = 0;

 struct modifier_storage ms = {0};
 // NOTE: This is all for TUI rendering.
 struct input_event recent_key = {0};
 struct tui_state state = {0};

 int32_t running = 1;
 // Note: This isn't really needed for operations, only useful for rendering.

 memoryArena clients_arena = {0};
 clients_arena.size = sizeof(struct client) * MAX_CLIENTS;
 clients_arena.memory = malloc(clients_arena.size);

 client_list clients = {0};
 client_list clients_free_list = {0};
 
 struct input_event read_events[MAX_READ_EVENTS];
 int32_t read_events_size = sizeof(struct input_event) * MAX_READ_EVENTS;

 printf("\x1b[?1049h");

 while (running) {
  uint64_t frame_start = __rdtsc(), frame_diff;
  memset(read_events, 0, read_events_size);

  int32_t accept_result = accept(socket_fd, NULL, NULL);
  if (accept_result == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      //printf("accept failed but with an acceptable error\n");
    } else {
      printf("panic everything is wrong\n");
      close(socket_fd);
      close(fd);
      unlink(path);
      free(clients_arena.memory);
      running = 0;
    }
  } else {
    if (newClient(&clients_arena, &clients, &clients_free_list, accept_result, POLLRDHUP)) {
      close(accept_result);
    }
  }

  int32_t read_bytes = read(fd, &read_events, read_events_size);
  int32_t num_valid_events = 0;
  struct event valid_events[MAX_READ_EVENTS];
  memset(valid_events, 0, sizeof(event) * MAX_READ_EVENTS);

  { // Read scope.
  uint64_t start = __rdtsc(), diff;

  if (read_bytes > 0) {
    int32_t amount_of_events = read_bytes / sizeof(struct input_event);
    state.read_amount = amount_of_events;
    for (int32_t i = 0; i < amount_of_events; i++) {

      struct input_event ev = read_events[i];

      if (ev.code == KEY_ESC && ev.value == 1 && (ms.modifiers[LSHIFT] || ms.modifiers[RSHIFT])) {
        printf("escaped \n");
        running = 0;
      }

      if (ev.type == EV_KEY) {
        switch (ev.value) {
          case (KEY_RELEASED): {
          if (handle_modifiers(ev.value, ev.code, &ms)) {
            break;
          }
          recent_key = ev; // TUI rendering.

          valid_events[num_valid_events].type = SDL_TYPE_KEY_UP;
          valid_events[num_valid_events].scancode = convertToSDLScanCode(ev.code);

          valid_events[num_valid_events].modstate = convertModiferState(&ms);

          num_valid_events++;
          } break;
          case(KEY_PRESSED): {
          if (handle_modifiers(ev.value, ev.code, &ms)) {
            break;
          }
          recent_key = ev; // TUI rendering.

          valid_events[num_valid_events].type = SDL_TYPE_KEY_UP;
          valid_events[num_valid_events].scancode = convertToSDLScanCode(ev.code);

          valid_events[num_valid_events].modstate = convertModiferState(&ms);

          num_valid_events++;
          } break;
          case(KEY_AUTOREPEAT): {

          } break;
          default: {
            printf("Unhandled key value. What do?\n");
          } break;
        }
      }

    }
  } 
  diff = __rdtsc() - start;
  state.read_cycle_count = diff;
  }

  { // Send Scope
  uint64_t start = __rdtsc(), diff;

  client *cur = clients.head;

  while (cur) { 
    client *next = cur->next;

    int32_t poll_result = poll(&cur->pfd, 1, 0); // poll current.
    if (poll_result > 0 && (cur->pfd.revents & POLLRDHUP)) {

    close(cur->pfd.fd);

    if (cur->prev) {
      cur->prev->next = cur->next;
    } else {
      clients.head = cur->next;
    }
    
    if (cur->next) {
      cur->next->prev = cur->prev;
    }
    
      client *free_cur = clients_free_list.head;
      client *free_prev = NULL;

    while (free_cur) {
      client *free_next = free_cur->next;

      free_prev = free_cur;
      free_cur = free_next;
    }
    
    if (free_prev) {
      free_prev->next = cur;
      cur->prev = free_prev;
    } else {
      clients_free_list.head = cur;
    }

    } else {
    // Hey the current one is valid so send them some of that sweet sweet input.
      if (num_valid_events > 0) {
      send(cur->pfd.fd, valid_events, sizeof(event) * num_valid_events, MSG_DONTWAIT);
      }
    }
    
    cur = next;
  }

  diff = __rdtsc() - start;
  state.send_cycle_count = diff;
  state.send_amount = num_valid_events; // Is this needed?
  if (num_valid_events > 0) {
   state.last_send_amount = num_valid_events;
  }
  }

  draw_tui(&ms, &recent_key, &clients, &state);

  frame_diff = __rdtsc() - frame_start;
  state.process_cycle_count = frame_diff;
}

 printf("\x1b[?1049l");

 printf("we broke\n");

 close(socket_fd);

 close(fd);

 unlink(path);
 free(clients_arena.memory);

 return 1;
}

// Returns 0 on success and 1 on failure.
int32_t newClient(memoryArena *arena, client_list *list, client_list *free_list, int fd, short int events) {

 int32_t result = 1;

 client *head = free_list->head;

 client *cur = list->head;
 client *prev = 0;
 while (cur) {
   prev = cur;
   cur = cur->next;
 }

 if (!head) {
  cur = pushSize(arena, sizeof(client));
 } else {
  cur = head;
  free_list->head = head->next;
 }
 
 if (cur) {
  cur->pfd.fd = fd;
  cur->pfd.events = events;
  if (prev) {
    prev->next = cur;
    cur->prev = prev;
  } else {
   list->head = cur;
  }
  result = 0;
 } else {
  result = 1;
 }

 return result;
}

void draw_tui(struct modifier_storage *ms, struct input_event *ev, client_list *list, tui_state* state) {
  uint64_t start = __rdtsc(), diff;
  // \033 is an octal escape.
  // TODO: I am become TUI master of text.
  printf("\033[1;1H\033[2J");
  printf("\033[0m");

  printf("| Control: ");
  if (ms->modifiers[LCTRL] || ms->modifiers[RCTRL]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }

  printf("\033[0m");
  printf("| Alt: ");
  if (ms->modifiers[LALT] || ms->modifiers[RALT]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }

  printf("\033[0m");
  printf("| Shift: ");
  if (ms->modifiers[LSHIFT] || ms->modifiers[RSHIFT]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }
  printf("\033[0m");
  printf("| Recent Valid | Key: %d | State %s | ", ev->code, ev->value ? "Pressed" : "Released");

  printf("\033[2;1H");
  
  client *cur = list->head;
  printf("| Clients | ");
  while (cur) {
   printf("\033[32m █ \033[0m");
   cur = cur->next;
  }
  
  printf("\033[3;1H");
  printf("Read Cycle Count: %ld | Read Amount %d | Send Cycle Count: %ld | Send Amount %d | Last Send Amount %d", 
         state->read_cycle_count, state->read_amount, state->send_cycle_count, state->send_amount, state->last_send_amount);

  printf("\033[4;1H");
  printf("| Process Loop Time %ld | Tui Time %ld", state->process_cycle_count, state->tui_cycle_count);
  printf("\033[6;1H");

  fflush(stdout);

  diff = __rdtsc() - start;
  state->tui_cycle_count = diff;
}

int32_t handle_modifiers(unsigned short value, unsigned short code, struct modifier_storage *ms) {
 int32_t result = 0;
 switch (code) {
   case KEY_LEFTCTRL: {
      ms->modifiers[LCTRL] = value;
      result = 1;
   } break;
   case KEY_LEFTALT: {
      ms->modifiers[LALT] = value;
      result = 1;
   } break;
   case KEY_LEFTSHIFT: {
      ms->modifiers[LSHIFT] = value;
      result = 1;
   } break;
   case KEY_RIGHTCTRL: {
      ms->modifiers[RCTRL] = value;
      result = 1;
   } break;
   case KEY_RIGHTALT: {
      ms->modifiers[RALT] = value;
      result = 1;
   } break;
   case KEY_RIGHTSHIFT: {
      ms->modifiers[RSHIFT] = value;
      result = 1;
   } break;
   case KEY_LEFTMETA: {
      ms->modifiers[LGUI] = value;
      result = 1;
   } break;
   case KEY_RIGHTMETA: {
      ms->modifiers[RGUI] = value;
      result = 1;
   } break;
   case KEY_NUMLOCK: {
    if (value) {
      ms->modifiers[NUM] = !ms->modifiers[NUM];
    }
    result = 1;
   } break;
   case KEY_CAPSLOCK: {
    if (value) {
      ms->modifiers[CAPS] = !ms->modifiers[CAPS];
    }
    result = 1;
   } break;
   case KEY_SCROLLLOCK: {
    if (value) {
      ms->modifiers[SCROLL] = !ms->modifiers[SCROLL];
    }
    result = 1;
   } break;
 }

 return result;
}

void *pushSize(memoryArena *arena, size_t size) {
 uint8_t *result = 0;
 if (arena->used + size < arena->size) {
  result = (uint8_t *)arena->memory + arena->used;

  arena->used += size;
 }
 return (void *)(result);
}

#define SDL_KMOD_LSHIFT 0x0001u
#define SDL_KMOD_RSHIFT 0x0002u
#define SDL_KMOD_LCTRL  0x0040u
#define SDL_KMOD_RCTRL  0x0080u
#define SDL_KMOD_LALT   0x0100u
#define SDL_KMOD_RALT   0x0200u
#define SDL_KMOD_LGUI   0x0400u
#define SDL_KMOD_RGUI   0x0800u
#define SDL_KMOD_CAPS   0x2000u
#define SDL_KMOD_SCROLL 0x8000u
#define SDL_KMOD_NUM    0x1000u

int16_t convertModiferState(struct modifier_storage * ms) {
 int16_t result = 0;
 if (ms->modifiers[LSHIFT]) {
  result |= SDL_KMOD_LSHIFT;
 }
 if (ms->modifiers[RSHIFT]) {
  result |= SDL_KMOD_RSHIFT;
 }
 if (ms->modifiers[LCTRL]) {
  result |= SDL_KMOD_LCTRL;
 }
 if (ms->modifiers[RCTRL]) {
  result |= SDL_KMOD_RCTRL;
 }
 if (ms->modifiers[LALT]) {
  result |= SDL_KMOD_LALT;
 }
 if (ms->modifiers[RALT]) {
  result |= SDL_KMOD_RALT;
 }
 if (ms->modifiers[LGUI]) {
  result |= SDL_KMOD_LGUI;
 }
 if (ms->modifiers[RGUI]) {
  result |= SDL_KMOD_RGUI;
 }
 if (ms->modifiers[CAPS]) {
  result |= SDL_KMOD_CAPS;
 }
 if (ms->modifiers[SCROLL]) {
  result |= SDL_KMOD_SCROLL;
 }
 if (ms->modifiers[NUM]) {
  result |= SDL_KMOD_NUM;
 }

 return result;
}

typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4,
    SDL_SCANCODE_B = 5,
    SDL_SCANCODE_C = 6,
    SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8,
    SDL_SCANCODE_F = 9,
    SDL_SCANCODE_G = 10,
    SDL_SCANCODE_H = 11,
    SDL_SCANCODE_I = 12,
    SDL_SCANCODE_J = 13,
    SDL_SCANCODE_K = 14,
    SDL_SCANCODE_L = 15,
    SDL_SCANCODE_M = 16,
    SDL_SCANCODE_N = 17,
    SDL_SCANCODE_O = 18,
    SDL_SCANCODE_P = 19,
    SDL_SCANCODE_Q = 20,
    SDL_SCANCODE_R = 21,
    SDL_SCANCODE_S = 22,
    SDL_SCANCODE_T = 23,
    SDL_SCANCODE_U = 24,
    SDL_SCANCODE_V = 25,
    SDL_SCANCODE_W = 26,
    SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Y = 28,
    SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_1 = 30,
    SDL_SCANCODE_2 = 31,
    SDL_SCANCODE_3 = 32,
    SDL_SCANCODE_4 = 33,
    SDL_SCANCODE_5 = 34,
    SDL_SCANCODE_6 = 35,
    SDL_SCANCODE_7 = 36,
    SDL_SCANCODE_8 = 37,
    SDL_SCANCODE_9 = 38,
    SDL_SCANCODE_0 = 39,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_MINUS = 45,
    SDL_SCANCODE_EQUALS = 46,
    SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49, 
    SDL_SCANCODE_NONUSHASH = 50,
    SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52,
    SDL_SCANCODE_GRAVE = 53, 
    SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55,
    SDL_SCANCODE_SLASH = 56,
    SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58,
    SDL_SCANCODE_F2 = 59,
    SDL_SCANCODE_F3 = 60,
    SDL_SCANCODE_F4 = 61,
    SDL_SCANCODE_F5 = 62,
    SDL_SCANCODE_F6 = 63,
    SDL_SCANCODE_F7 = 64,
    SDL_SCANCODE_F8 = 65,
    SDL_SCANCODE_F9 = 66,
    SDL_SCANCODE_F10 = 67,
    SDL_SCANCODE_F11 = 68,
    SDL_SCANCODE_F12 = 69,
    SDL_SCANCODE_PRINTSCREEN = 70,
    SDL_SCANCODE_SCROLLLOCK = 71,
    SDL_SCANCODE_PAUSE = 72,
    SDL_SCANCODE_INSERT = 73,
    SDL_SCANCODE_HOME = 74,
    SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_DELETE = 76,
    SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_NUMLOCKCLEAR = 83,
    SDL_SCANCODE_KP_DIVIDE = 84,
    SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_KP_PLUS = 87,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89,
    SDL_SCANCODE_KP_2 = 90,
    SDL_SCANCODE_KP_3 = 91,
    SDL_SCANCODE_KP_4 = 92,
    SDL_SCANCODE_KP_5 = 93,
    SDL_SCANCODE_KP_6 = 94,
    SDL_SCANCODE_KP_7 = 95,
    SDL_SCANCODE_KP_8 = 96,
    SDL_SCANCODE_KP_9 = 97,
    SDL_SCANCODE_KP_0 = 98,
    SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_NONUSBACKSLASH = 100,
    SDL_SCANCODE_APPLICATION = 101,
    SDL_SCANCODE_POWER = 102,
    SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_F13 = 104,
    SDL_SCANCODE_F14 = 105,
    SDL_SCANCODE_F15 = 106,
    SDL_SCANCODE_F16 = 107,
    SDL_SCANCODE_F17 = 108,
    SDL_SCANCODE_F18 = 109,
    SDL_SCANCODE_F19 = 110,
    SDL_SCANCODE_F20 = 111,
    SDL_SCANCODE_F21 = 112,
    SDL_SCANCODE_F22 = 113,
    SDL_SCANCODE_F23 = 114,
    SDL_SCANCODE_F24 = 115,
    SDL_SCANCODE_EXECUTE = 116,
    SDL_SCANCODE_HELP = 117,
    SDL_SCANCODE_MENU = 118,
    SDL_SCANCODE_SELECT = 119,
    SDL_SCANCODE_STOP = 120, 
    SDL_SCANCODE_AGAIN = 121,
    SDL_SCANCODE_UNDO = 122,
    SDL_SCANCODE_CUT = 123,
    SDL_SCANCODE_COPY = 124,
    SDL_SCANCODE_PASTE = 125,
    SDL_SCANCODE_FIND = 126,
    SDL_SCANCODE_MUTE = 127,
    SDL_SCANCODE_VOLUMEUP = 128,
    SDL_SCANCODE_VOLUMEDOWN = 129,
    SDL_SCANCODE_KP_COMMA = 133,
    SDL_SCANCODE_KP_EQUALSAS400 = 134,
    SDL_SCANCODE_INTERNATIONAL1 = 135,
    SDL_SCANCODE_INTERNATIONAL2 = 136,
    SDL_SCANCODE_INTERNATIONAL3 = 137,
    SDL_SCANCODE_INTERNATIONAL4 = 138,
    SDL_SCANCODE_INTERNATIONAL5 = 139,
    SDL_SCANCODE_INTERNATIONAL6 = 140,
    SDL_SCANCODE_INTERNATIONAL7 = 141,
    SDL_SCANCODE_INTERNATIONAL8 = 142,
    SDL_SCANCODE_INTERNATIONAL9 = 143,
    SDL_SCANCODE_LANG1 = 144,
    SDL_SCANCODE_LANG2 = 145,
    SDL_SCANCODE_LANG3 = 146,
    SDL_SCANCODE_LANG4 = 147,
    SDL_SCANCODE_LANG5 = 148,
    SDL_SCANCODE_LANG6 = 149,
    SDL_SCANCODE_LANG7 = 150,
    SDL_SCANCODE_LANG8 = 151,
    SDL_SCANCODE_LANG9 = 152,
    SDL_SCANCODE_ALTERASE = 153,
    SDL_SCANCODE_SYSREQ = 154,
    SDL_SCANCODE_CANCEL = 155,
    SDL_SCANCODE_CLEAR = 156,
    SDL_SCANCODE_PRIOR = 157,
    SDL_SCANCODE_RETURN2 = 158,
    SDL_SCANCODE_SEPARATOR = 159,
    SDL_SCANCODE_OUT = 160,
    SDL_SCANCODE_OPER = 161,
    SDL_SCANCODE_CLEARAGAIN = 162,
    SDL_SCANCODE_CRSEL = 163,
    SDL_SCANCODE_EXSEL = 164,
    SDL_SCANCODE_FRONT = 165,
    SDL_SCANCODE_KP_00 = 176,
    SDL_SCANCODE_KP_000 = 177,
    SDL_SCANCODE_THOUSANDSSEPARATOR = 178,
    SDL_SCANCODE_DECIMALSEPARATOR = 179,
    SDL_SCANCODE_CURRENCYUNIT = 180,
    SDL_SCANCODE_CURRENCYSUBUNIT = 181,
    SDL_SCANCODE_KP_LEFTPAREN = 182,
    SDL_SCANCODE_KP_RIGHTPAREN = 183,
    SDL_SCANCODE_KP_LEFTBRACE = 184,
    SDL_SCANCODE_KP_RIGHTBRACE = 185,
    SDL_SCANCODE_KP_TAB = 186,
    SDL_SCANCODE_KP_BACKSPACE = 187,
    SDL_SCANCODE_KP_A = 188,
    SDL_SCANCODE_KP_B = 189,
    SDL_SCANCODE_KP_C = 190,
    SDL_SCANCODE_KP_D = 191,
    SDL_SCANCODE_KP_E = 192,
    SDL_SCANCODE_KP_F = 193,
    SDL_SCANCODE_KP_XOR = 194,
    SDL_SCANCODE_KP_POWER = 195,
    SDL_SCANCODE_KP_PERCENT = 196,
    SDL_SCANCODE_KP_LESS = 197,
    SDL_SCANCODE_KP_GREATER = 198,
    SDL_SCANCODE_KP_AMPERSAND = 199,
    SDL_SCANCODE_KP_DBLAMPERSAND = 200,
    SDL_SCANCODE_KP_VERTICALBAR = 201,
    SDL_SCANCODE_KP_DBLVERTICALBAR = 202,
    SDL_SCANCODE_KP_COLON = 203,
    SDL_SCANCODE_KP_HASH = 204,
    SDL_SCANCODE_KP_SPACE = 205,
    SDL_SCANCODE_KP_AT = 206,
    SDL_SCANCODE_KP_EXCLAM = 207,
    SDL_SCANCODE_KP_MEMSTORE = 208,
    SDL_SCANCODE_KP_MEMRECALL = 209,
    SDL_SCANCODE_KP_MEMCLEAR = 210,
    SDL_SCANCODE_KP_MEMADD = 211,
    SDL_SCANCODE_KP_MEMSUBTRACT = 212,
    SDL_SCANCODE_KP_MEMMULTIPLY = 213,
    SDL_SCANCODE_KP_MEMDIVIDE = 214,
    SDL_SCANCODE_KP_PLUSMINUS = 215,
    SDL_SCANCODE_KP_CLEAR = 216,
    SDL_SCANCODE_KP_CLEARENTRY = 217,
    SDL_SCANCODE_KP_BINARY = 218,
    SDL_SCANCODE_KP_OCTAL = 219,
    SDL_SCANCODE_KP_DECIMAL = 220,
    SDL_SCANCODE_KP_HEXADECIMAL = 221,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_LGUI = 227,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_SCANCODE_RGUI = 231,
    SDL_SCANCODE_MODE = 257,
    SDL_SCANCODE_SLEEP = 258,
    SDL_SCANCODE_WAKE = 259,
    SDL_SCANCODE_CHANNEL_INCREMENT = 260,
    SDL_SCANCODE_CHANNEL_DECREMENT = 261,
    SDL_SCANCODE_MEDIA_PLAY = 262,
    SDL_SCANCODE_MEDIA_PAUSE = 263,
    SDL_SCANCODE_MEDIA_RECORD = 264,
    SDL_SCANCODE_MEDIA_FAST_FORWARD = 265,
    SDL_SCANCODE_MEDIA_REWIND = 266,
    SDL_SCANCODE_MEDIA_NEXT_TRACK = 267,
    SDL_SCANCODE_MEDIA_PREVIOUS_TRACK = 268,
    SDL_SCANCODE_MEDIA_STOP = 269,
    SDL_SCANCODE_MEDIA_EJECT = 270,
    SDL_SCANCODE_MEDIA_PLAY_PAUSE = 271,
    SDL_SCANCODE_MEDIA_SELECT = 272,
    SDL_SCANCODE_AC_NEW = 273, 
    SDL_SCANCODE_AC_OPEN = 274,
    SDL_SCANCODE_AC_CLOSE = 275,
    SDL_SCANCODE_AC_EXIT = 276,
    SDL_SCANCODE_AC_SAVE = 277,
    SDL_SCANCODE_AC_PRINT = 278,
    SDL_SCANCODE_AC_PROPERTIES = 279,
    SDL_SCANCODE_AC_SEARCH = 280,
    SDL_SCANCODE_AC_HOME = 281,
    SDL_SCANCODE_AC_BACK = 282,
    SDL_SCANCODE_AC_FORWARD = 283,
    SDL_SCANCODE_AC_STOP = 284,
    SDL_SCANCODE_AC_REFRESH = 285,
    SDL_SCANCODE_AC_BOOKMARKS = 286,
    SDL_SCANCODE_SOFTLEFT = 287,
    SDL_SCANCODE_SOFTRIGHT = 288,
    SDL_SCANCODE_CALL = 289,
    SDL_SCANCODE_ENDCALL = 290,
    SDL_SCANCODE_RESERVED = 400,
    SDL_SCANCODE_COUNT = 512
} SDL_Scancode;

int32_t convertToSDLScanCode(uint16_t scancode) {
 switch (scancode) {
  case KEY_ESC: return SDL_SCANCODE_ESCAPE; 
  case KEY_1:	return SDL_SCANCODE_1;
  case KEY_2:	return SDL_SCANCODE_2;
  case KEY_3: return SDL_SCANCODE_3;
  case KEY_4: return SDL_SCANCODE_4;
  case KEY_5: return SDL_SCANCODE_5;
  case KEY_6: return SDL_SCANCODE_6;
  case KEY_7: return SDL_SCANCODE_7;
  case KEY_8: return SDL_SCANCODE_8;
  case KEY_9: return SDL_SCANCODE_9;
  case KEY_0: return SDL_SCANCODE_0;
  case KEY_MINUS: return SDL_SCANCODE_MINUS;
  case KEY_EQUAL: return SDL_SCANCODE_EQUALS;
  case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
  case KEY_TAB: return SDL_SCANCODE_TAB;
  case KEY_Q: return SDL_SCANCODE_Q;
  case KEY_W: return SDL_SCANCODE_W;
  case KEY_E: return SDL_SCANCODE_E;
  case KEY_R: return SDL_SCANCODE_R;
  case KEY_T: return SDL_SCANCODE_T;
  case KEY_Y: return SDL_SCANCODE_Y;
  case KEY_U: return SDL_SCANCODE_U;
  case KEY_I: return SDL_SCANCODE_I;
  case KEY_O: return SDL_SCANCODE_O;
  case KEY_P: return SDL_SCANCODE_P;
  case KEY_LEFTBRACE: return SDL_SCANCODE_LEFTBRACKET;
  case KEY_RIGHTBRACE: return SDL_SCANCODE_RIGHTBRACKET;
  case KEY_ENTER: return SDL_SCANCODE_RETURN;
  case KEY_LEFTCTRL: return SDL_SCANCODE_LCTRL;
  case KEY_A: return SDL_SCANCODE_A;
  case KEY_S: return SDL_SCANCODE_S;
  case KEY_D: return SDL_SCANCODE_D;
  case KEY_F: return SDL_SCANCODE_F;
  case KEY_G: return SDL_SCANCODE_G;
  case KEY_H: return SDL_SCANCODE_H;
  case KEY_J: return SDL_SCANCODE_J;
  case KEY_K: return SDL_SCANCODE_K;
  case KEY_L: return SDL_SCANCODE_L;
  case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
  case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
  case KEY_GRAVE: return SDL_SCANCODE_GRAVE;
  case KEY_LEFTSHIFT: return SDL_SCANCODE_LSHIFT;
  case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
  case KEY_Z: return SDL_SCANCODE_Z;
  case KEY_X: return SDL_SCANCODE_X;
  case KEY_C: return SDL_SCANCODE_C;
  case KEY_V: return SDL_SCANCODE_V;
  case KEY_B: return SDL_SCANCODE_B;
  case KEY_N: return SDL_SCANCODE_N;
  case KEY_M: return SDL_SCANCODE_M;
  case KEY_COMMA: return SDL_SCANCODE_COMMA;
  case KEY_DOT: return SDL_SCANCODE_PERIOD;
  case KEY_SLASH: return SDL_SCANCODE_SLASH;
  case KEY_RIGHTSHIFT: return SDL_SCANCODE_RSHIFT;
  case KEY_KPASTERISK: return SDL_SCANCODE_KP_MULTIPLY;
  case KEY_LEFTALT: return SDL_SCANCODE_LALT;
  case KEY_SPACE: return SDL_SCANCODE_SPACE;
  case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
  case KEY_F1: return SDL_SCANCODE_F1;
  case KEY_F2: return SDL_SCANCODE_F2;
  case KEY_F3: return SDL_SCANCODE_F3;
  case KEY_F4: return SDL_SCANCODE_F4;
  case KEY_F5: return SDL_SCANCODE_F5;
  case KEY_F6: return SDL_SCANCODE_F6;
  case KEY_F7: return SDL_SCANCODE_F7;
  case KEY_F8: return SDL_SCANCODE_F8;
  case KEY_F9: return SDL_SCANCODE_F9;
  case KEY_F10: return SDL_SCANCODE_F10;
  case KEY_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
  case KEY_SCROLLLOCK: return SDL_SCANCODE_SCROLLLOCK;
  case KEY_KP7: return SDL_SCANCODE_KP_7;
  case KEY_KP8: return SDL_SCANCODE_KP_8;
  case KEY_KP9: return SDL_SCANCODE_KP_9;
  case KEY_KPMINUS: return SDL_SCANCODE_KP_MINUS;
  case KEY_KP4: return SDL_SCANCODE_KP_4;
  case KEY_KP5: return SDL_SCANCODE_KP_5;
  case KEY_KP6: return SDL_SCANCODE_KP_6;
  case KEY_KPPLUS: return SDL_SCANCODE_KP_PLUS;
  case KEY_KP1: return SDL_SCANCODE_KP_1;
  case KEY_KP2: return SDL_SCANCODE_KP_2;
  case KEY_KP3: return SDL_SCANCODE_KP_3;
  case KEY_KP0: return SDL_SCANCODE_KP_0;
  case KEY_KPDOT: return SDL_SCANCODE_KP_PERIOD;
  case KEY_102ND: return SDL_SCANCODE_NONUSBACKSLASH;
  case KEY_F11: return SDL_SCANCODE_F11;
  case KEY_F12: return SDL_SCANCODE_F12;
  case KEY_KATAKANA: return SDL_SCANCODE_LANG3;
  case KEY_HIRAGANA: return SDL_SCANCODE_LANG4;
  case KEY_HENKAN: return SDL_SCANCODE_INTERNATIONAL5;
  case KEY_KATAKANAHIRAGANA: return SDL_SCANCODE_LANG5;
  case KEY_MUHENKAN: return SDL_SCANCODE_INTERNATIONAL4;
  case KEY_KPENTER: return SDL_SCANCODE_KP_ENTER;
  case KEY_RIGHTCTRL: return SDL_SCANCODE_RCTRL;
  case KEY_KPSLASH: return SDL_SCANCODE_KP_DIVIDE;
  case KEY_SYSRQ: return SDL_SCANCODE_PRINTSCREEN;
  case KEY_RIGHTALT: return SDL_SCANCODE_RALT;
  case KEY_HOME: return SDL_SCANCODE_HOME;
  case KEY_UP: return SDL_SCANCODE_UP;
  case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
  case KEY_LEFT: return SDL_SCANCODE_LEFT;
  case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
  case KEY_END: return SDL_SCANCODE_END;
  case KEY_DOWN: return SDL_SCANCODE_DOWN;
  case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
  case KEY_INSERT: return SDL_SCANCODE_INSERT;
  case KEY_DELETE: return SDL_SCANCODE_DELETE;
  case KEY_MUTE: return SDL_SCANCODE_MUTE;
  case KEY_VOLUMEDOWN: return SDL_SCANCODE_VOLUMEDOWN;
  case KEY_VOLUMEUP: return SDL_SCANCODE_VOLUMEUP;
  case KEY_POWER: return SDL_SCANCODE_POWER;
  case KEY_KPEQUAL: return SDL_SCANCODE_KP_EQUALS;
  case KEY_KPPLUSMINUS: return SDL_SCANCODE_KP_PLUSMINUS;
  case KEY_PAUSE: return SDL_SCANCODE_PAUSE;
  case KEY_KPCOMMA: return SDL_SCANCODE_KP_COMMA;
  case KEY_HANGEUL: return SDL_SCANCODE_LANG1;
  case KEY_HANJA: return SDL_SCANCODE_LANG2;
  case KEY_YEN: return SDL_SCANCODE_INTERNATIONAL3;
  case KEY_LEFTMETA: return SDL_SCANCODE_LGUI;
  case KEY_RIGHTMETA: return SDL_SCANCODE_RGUI;
  case KEY_COMPOSE: return SDL_SCANCODE_APPLICATION;
  case KEY_STOP: return SDL_SCANCODE_STOP;
  case KEY_AGAIN: return SDL_SCANCODE_AGAIN;
  case KEY_UNDO: return SDL_SCANCODE_UNDO;
  case KEY_FRONT: return SDL_SCANCODE_FRONT;
  case KEY_COPY: return SDL_SCANCODE_COPY;
  case KEY_PASTE: return SDL_SCANCODE_PASTE;
  case KEY_FIND: return SDL_SCANCODE_FIND;
  case KEY_CUT: return SDL_SCANCODE_CUT;
  case KEY_HELP: return SDL_SCANCODE_HELP;
  case KEY_MENU: return SDL_SCANCODE_MENU;
  case KEY_SLEEP: return SDL_SCANCODE_SLEEP;
  case KEY_WAKEUP: return SDL_SCANCODE_WAKE;
  case KEY_HOMEPAGE: return SDL_SCANCODE_AC_HOME;
  case KEY_REFRESH: return SDL_SCANCODE_AC_REFRESH;
  case KEY_EXIT: return SDL_SCANCODE_AC_EXIT;
  case KEY_NEW: return SDL_SCANCODE_AC_NEW;
  case KEY_F13: return SDL_SCANCODE_F13;
  case KEY_F14: return SDL_SCANCODE_F14;
  case KEY_F15: return SDL_SCANCODE_F15;
  case KEY_F16: return SDL_SCANCODE_F16;
  case KEY_F17: return SDL_SCANCODE_F17;
  case KEY_F18: return SDL_SCANCODE_F18;
  case KEY_F19: return SDL_SCANCODE_F19;
  case KEY_F20: return SDL_SCANCODE_F20;
  case KEY_F21: return SDL_SCANCODE_F21;
  case KEY_F22: return SDL_SCANCODE_F22;
  case KEY_F23: return SDL_SCANCODE_F23;
  case KEY_F24: return SDL_SCANCODE_F24;
  case KEY_PLAY: return SDL_SCANCODE_MEDIA_PLAY;
  case KEY_FASTFORWARD: return SDL_SCANCODE_MEDIA_FAST_FORWARD;
  case KEY_PRINT: return SDL_SCANCODE_PRINTSCREEN;
  case KEY_SEARCH: return SDL_SCANCODE_AC_SEARCH;
  case KEY_CANCEL: return SDL_SCANCODE_CANCEL;
  case KEY_VIDEO_NEXT: return SDL_SCANCODE_MEDIA_NEXT_TRACK;
  case KEY_VIDEO_PREV: return SDL_SCANCODE_MEDIA_PREVIOUS_TRACK;
  default: return SDL_SCANCODE_UNKNOWN;
 }
}
