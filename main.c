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

/*
 * SDL scancode definitions - Used for converting platform key input to sdl key input
 *
 * Source:
 * https://wiki.libsdl.org/SDL3/SDL_Scancode
 *
 * Copyright (c) 2024 SDL
 * Licensed under the zlib license.
*/

#define KEY_RELEASED 0
#define KEY_PRESSED 1
#define KEY_AUTOREPEAT 2

#define NUM_OF_MODIFIERS 3

#define Kilobytes(number) ((number) * 1024ull)
#define Megabytes(number) (Kilobytes(number) * 1024ull)
#define Gigabytes(number) (Megabytes(number) * 1024ull)

enum modifier_position {
 CONTROL,
 ALT,
 SHIFT,
};

struct modifier_storage {
 int modifiers[NUM_OF_MODIFIERS];
};

int handle_modifiers(unsigned short value, unsigned short code, struct modifier_storage *ms) {

 int result = 0;
 switch (code) {
   case KEY_LEFTCTRL: {
      ms->modifiers[CONTROL] = value;
      result = 1;
   } break;
   case KEY_LEFTALT: {
      ms->modifiers[ALT] = value;
      result = 1;
   } break;
   case KEY_LEFTSHIFT: {
      ms->modifiers[SHIFT] = value;
      result = 1;
   } break;
   case KEY_RIGHTCTRL: {
      ms->modifiers[CONTROL] = value;
      result = 1;
   } break;
   case KEY_RIGHTALT: {
      ms->modifiers[ALT] = value;
      result = 1;
   } break;
   case KEY_RIGHTSHIFT: {
      ms->modifiers[SHIFT] = value;
      result = 1;
   } break;
 }

 return result;
}


#define DRAW_TUI 1

const char *path = "/run/user/1000/keyboardListener";

typedef struct memoryArena {
 uint32_t size;
 uint32_t used;

 void *memory;
} memoryArena;

void *pushSize(memoryArena *arena, size_t size) {
 uint8_t *result = 0;
 if (arena->used + size < arena->size) {
  result = (uint8_t *)arena->memory + arena->used;

  arena->used += size;
 }
 return (void *)(result);
}

#define MAX_CLIENTS 64

typedef struct client {
 struct pollfd pfd;

 struct client *next;
 struct client *prev;
} client;
typedef struct client_list {
 client *head;
} client_list;

void draw_tui(struct modifier_storage *ms, struct input_event *ev, client_list *list) {
  // \033 is an octal escape.
  // TODO: I am become TUI master of text.
  printf("\033[1;1H\033[2J");
  printf("\033[0m");

  printf("| Control: ");
  if (ms->modifiers[CONTROL]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }

  printf("\033[0m");
  printf("| Alt: ");
  if (ms->modifiers[ALT]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }

  printf("\033[0m");
  printf("| Shift: ");
  if (ms->modifiers[SHIFT]) {
    printf("\033[32m");
    printf("█ ");
  } else {
    printf("\033[31m");
    printf("░ ");
  }
  printf("\033[0m");
  printf("| Recent Valid | Key: %d | State %s | ", ev->code, ev->value ? "Pressed" : "Released");
  printf("\n");

  client *cur = list->head;
  printf("| Clients | ");
  while (cur) {
   printf("\033[32m █ \033[0m");
   cur = cur->next;
  }
  
  printf("\n");
  fflush(stdout);
}
// Returns 0 on success and 1 on failure.
int newNode(memoryArena *arena, client_list *list, client_list *free_list, int fd, short int events) {

 int result = 0;

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

int main() {

 remove(path);
 
 struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };
 strcpy(sockaddr.sun_path, path);

 int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
 if (socket_fd == -1) {
  printf("Unable to open socket\n");
  return(-1);
 }
 
 int bind_result = bind(socket_fd, (struct sockaddr *)&sockaddr, offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1);
 if (bind_result == -1) {
  printf("Couldn't assign a name to a socket\n");
  close(socket_fd);
  return(-1);
 }
 
 int listen_result = listen(socket_fd, 1);
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
  int setflag_result = fcntl(socket_fd, F_SETFD, O_NONBLOCK);
  if (setflag_result == -1) {
   printf("Couldn't set socket to non blocking\n");
   close(socket_fd);
   unlink(path);
   return(-1);
  } 
 }


 struct pollfd pollingfd = {0};
 pollingfd.fd = socket_fd;
 pollingfd.events = POLLIN;

 // TODO: Find keyboard from ID's.
 int fd = open("/dev/input/event3", O_RDONLY);
 if (fd == -1) {
  printf("Unable to access keyboard\n");
  close(socket_fd);
  unlink(path);
  return(-1);
 }

 {
  int setflag_result = fcntl(fd, F_SETFD, O_NONBLOCK);
  if (setflag_result == -1) {
   printf("Couldn't set socket to non blocking\n");
   close(socket_fd);
   close(fd);
   unlink(path);
   return(-1);
  } 
 }
 printf("File Descriptor: %d \n", fd);

 struct modifier_storage ms;
 ms.modifiers[CONTROL] = 0;
 ms.modifiers[ALT] = 0;
 ms.modifiers[SHIFT] = 0;

 int running = 1;

 int listener_fd = 0;
 // Note: This isn't really needed for operations, only useful for rendering.
 struct input_event recent_key = {0};

 memoryArena clients_arena;
 clients_arena.size = sizeof(struct client) * MAX_CLIENTS;
 clients_arena.memory = malloc(clients_arena.size);

 struct client_list clients = {0};
 struct client_list free_list = {0};

 if (DRAW_TUI) {
  printf("\x1b[?1049h");
 }

 while (running) {

  int poll_result = poll(&pollingfd, 1, 0);
  if (poll_result > 0) { 
   if (pollingfd.revents & POLLIN) {
    int accept_result = accept(socket_fd, NULL, NULL);
    if (accept_result == -1) {
     if (errno == EAGAIN || errno == EWOULDBLOCK) {
      printf("accept failed but with an acceptable error\n");
     } else {
      printf("panic everything is wrong\n");
      close(socket_fd);
      close(fd);
      unlink(path);
      free(clients_arena.memory);
      running = 0;
     }
    } else {
     // To free a client slot we would have to poll the other fd which
     // Seems like more effort than I want to do right now.
     if (newNode(&clients_arena, &clients, &free_list, accept_result, POLLRDHUP)) {
      close(accept_result);
     }
    }

   }
  } else if (poll_result == -1) {
   printf("there was some type of error\n");
   printf("but I don't want to check errno\n");
  }

  {
    client *cur = clients.head;

    while (cur) {

      client *next = cur->next;

      int poll_result = poll(&cur->pfd, 1, 0); // poll current.
      if (poll_result > 0 && (cur->pfd.revents & POLLRDHUP)) {

      close(cur->pfd.fd);

      if (cur->prev) {
        cur->prev->next = cur->next;
      } else {
        clients.head = cur->next;
        printf("%p\n", clients.head);
      }
      
      if (cur->next) {
        cur->next->prev = cur->prev;
      }
      
      client *free_next = free_list.head;
      client *free_prev = 0;

      while (free_next) {
        free_next = free_next->next;
        free_prev = free_next->prev;
      }
      
      if (free_prev) {
        free_prev->next = cur;
        cur->prev = free_prev;
      } else {
        free_list.head = cur;
      }

      } else {
      // Hey the current one is valid so send them some of that sweet sweet input.
      }
      
      cur = next;
    }
  
  struct input_event ev = {0};
  read(fd, &ev, sizeof(struct input_event));

  if (DRAW_TUI) {
   draw_tui(&ms, &recent_key, &clients);
  } else {
   if (ev.type == EV_KEY) {
    if (ev.value == KEY_PRESSED) {
     //printf("pressed: ev@code %d | ev@type %d\n", ev.code, ev.type);
    }
    if (ev.value == KEY_RELEASED) {
     //printf("released: ev@code %d | ev@type %d\n", ev.code, ev.type);
    }
   }
  }

  if (ev.code == KEY_ESC && ev.value == 1 && ms.modifiers[SHIFT]) {
    printf("escaped \n");
    running = 0;
  }

  if (ev.type == EV_KEY) {
   switch (ev.value) {
    case (KEY_RELEASED): {
     if (handle_modifiers(ev.value, ev.code, &ms)) {
      break;
     }
     recent_key = ev;
    } break;
    case(KEY_PRESSED): {
     if (handle_modifiers(ev.value, ev.code, &ms)) {
      break;
     }
     recent_key = ev;
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
 if (DRAW_TUI) {
  printf("\x1b[?1049l");
 }

 printf("we broke\n");

 close(socket_fd);
 close(fd);
 unlink(path);
 free(clients_arena.memory);

 return 1;
}
