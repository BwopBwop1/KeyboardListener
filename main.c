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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/time.h>
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

#define NUM_OF_MODIFIERS 6
enum modifier_position {
 CONTROL,
 ALT,
 SHIFT,
};

struct modifier_storage {
 int modifiers[3];
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

void draw_tui(struct modifier_storage *ms, struct input_event *ev, int client_fd) {
  // \033 is an octal escape.
  printf("\033[2J\033[H");
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
  printf("| Recent Valid | Key: %d | State %s | Client %d\n", ev->code, ev->value ? "Pressed" : "Released", client_fd);

}

#define DRAW_TUI 1

const char *path = "/run/user/1000/keyboardListener";

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
 while (running) {

  int poll_result = poll(&pollingfd, 1, 0);
  if (poll_result > 0) { // We have some activity.
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
      running = 0;
     }
    } else {
     if (listener_fd == 0) {
      listener_fd = accept_result;
     }
    }

    if (pollingfd.revents & POLLIN) {
      printf("there is a revent connection to read\n");
    }
   }
  } else if (poll_result == -1) {
   printf("there was some type of error\n");
   printf("but I don't want to check errno\n");
  }

  struct input_event ev = {0};
  read(fd, &ev, sizeof(struct input_event));
  
  if (DRAW_TUI) {
   draw_tui(&ms, &recent_key, listener_fd);
  } else {
   if (ev.type == EV_KEY) {
    if (ev.value == KEY_PRESSED) {
     printf("pressed: ev@code %d | ev@type %d\n", ev.code, ev.type);
    }
    if (ev.value == KEY_RELEASED) {
     printf("released: ev@code %d | ev@type %d\n", ev.code, ev.type);
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

 printf("we broke\n");

 close(socket_fd);
 close(fd);
 unlink(path);

 return 1;
}
