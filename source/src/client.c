#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum ClientConstants { MAX_BUFFER = 2048, EOT_MARKER = '\4', BASE_10 = 10 };

// cppcheck-suppress constParameter
int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  const char *server_ip = argv[1];
  uint16_t port = (uint16_t)strtol(argv[2], NULL, BASE_10);

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket failed");
    return EXIT_FAILURE;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
    fprintf(stderr, "Invalid address/ Address not supported\n");
    close(sockfd);
    return EXIT_FAILURE;
  }

  if (connect(sockfd, (struct sockaddr *)&server_addr,
              (socklen_t)sizeof(server_addr)) < 0) {
    perror("Connection Failed");
    close(sockfd);
    return EXIT_FAILURE;
  }

  printf("Connected to server %s:%d\n", server_ip, port);
  char cmd_buf[MAX_BUFFER];

  while (1) {
    printf("shell> ");
    fflush(stdout);

    if (fgets(cmd_buf, MAX_BUFFER, stdin) == NULL) {
      break;
    }

    cmd_buf[strcspn(cmd_buf, "\n")] = 0;
    size_t len = strlen(cmd_buf);

    if (len == 0) {
      continue;
    }

    if (write(sockfd, cmd_buf, len) < 0) {
      perror("write failed");
      break;
    }

    if (strcmp(cmd_buf, "exit") == 0) {
      break;
    }

    char read_buf[MAX_BUFFER];
    while (1) {
      ssize_t rn = read(sockfd, read_buf, MAX_BUFFER);
      if (rn <= 0) {
        break;
      }

      int eot_found = 0;
      for (ssize_t i = 0; i < rn; i++) {
        if (read_buf[i] == (char)EOT_MARKER) {
          eot_found = 1;
          if (i > 0) {
            if (write(STDOUT_FILENO, read_buf, (size_t)i) < 0) {
              perror("write stdout failed");
            }
          }
          break;
        }
      }

      if (eot_found) {
        break;
      }

      if (write(STDOUT_FILENO, read_buf, (size_t)rn) < 0) {
        perror("write stdout failed");
        break;
      }
    }
  }

  printf("Closing connection.\n");
  close(sockfd);
  return EXIT_SUCCESS;
}
