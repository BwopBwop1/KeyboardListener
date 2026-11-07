
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

          num_valid_events++;
          } break;
          case(KEY_PRESSED): {
          if (handle_modifiers(ev.value, ev.code, &ms)) {
            break;
          }
          recent_key = ev; // TUI rendering.

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

  draw_tui(&ms, &recent_key, &clients, &state);

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
  printf("| Read Cycle Count: %ld | Read Amount %d | Send Cycle Count: %ld | Send Amount %d | Last Send Amount %d", 
         state->read_cycle_count, state->read_amount, state->send_cycle_count, state->send_amount, state->last_send_amount);

  printf("\033[4;1H");
  printf("| Process Loop Time %ld | Tui Time %ld |", state->process_cycle_count, state->tui_cycle_count);
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
