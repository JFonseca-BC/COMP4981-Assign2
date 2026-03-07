#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum ServerConstants {
  MAX_BUFFER = 2048,
  EOT_MARKER = '\4',
  BACKLOG_SIZE = 10,
  MAX_ARGS = 100,
  MAX_ARGS_LIMIT = 99,
  DEFAULT_FILE_PERMS = 0644,
  MAX_PATH_LEN = 256,
  BASE_10 = 10
};

/* Reaps zombie processes to prevent resource leaks */
static void sigchld_handler(int s) {
  int saved_errno = errno;
  (void)s;
  while (waitpid(-1, NULL, WNOHANG) > 0) {
    /* loop and reap all dead children */
  }
  errno = saved_errno;
}

static int setup_server(uint16_t port) {
  int server_fd;
  struct sockaddr_in server_addr;
  int opt = 1;

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                 (socklen_t)sizeof(opt)) < 0) {
    perror("setsockopt failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr *)&server_addr,
           (socklen_t)sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  if (listen(server_fd, BACKLOG_SIZE) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  return server_fd;
}

static void execute_command(char *cmd_string) {
  char *argv[MAX_ARGS];
  int argc_cmd = 0;

  const char *in_file = NULL;
  const char *out_file = NULL;
  const char *err_file = NULL;
  char *saveptr;

  char *token = strtok_r(cmd_string, " ", &saveptr);
  while (token != NULL && argc_cmd < MAX_ARGS_LIMIT) {
    if (strcmp(token, "<") == 0) {
      in_file = strtok_r(NULL, " ", &saveptr);
    } else if (strcmp(token, ">") == 0) {
      out_file = strtok_r(NULL, " ", &saveptr);
    } else if (strcmp(token, "2>") == 0) {
      err_file = strtok_r(NULL, " ", &saveptr);
    } else {
      argv[argc_cmd++] = token;
    }
    token = strtok_r(NULL, " ", &saveptr);
  }
  argv[argc_cmd] = NULL;

  if (argc_cmd == 0) {
    exit(EXIT_SUCCESS);
  }

  if (in_file) {
    int fd = open(in_file, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      close(fd);
    }
  }
  if (out_file) {
    int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  DEFAULT_FILE_PERMS);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
  }
  if (err_file) {
    int fd = open(err_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  DEFAULT_FILE_PERMS);
    if (fd >= 0) {
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
  }

  if (argv[0][0] == '/' || argv[0][0] == '.') {
    execv(argv[0], argv);
  } else {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    execv(path, argv);
    snprintf(path, sizeof(path), "/usr/bin/%s", argv[0]);
    execv(path, argv);
  }

  perror("execv failed");
  exit(EXIT_FAILURE);
}

static void process_client(int client_fd) {
  char cmd_buf[MAX_BUFFER];

  while (1) {
    memset(cmd_buf, 0, MAX_BUFFER);
    ssize_t bytes_read = read(client_fd, cmd_buf, MAX_BUFFER - 1);

    if (bytes_read <= 0) {
      break;
    }

    cmd_buf[strcspn(cmd_buf, "\r\n")] = 0;

    if (strcmp(cmd_buf, "exit") == 0) {
      break;
    }

    int out_pipe[2];
    if (pipe(out_pipe) < 0) { // NOLINT(android-cloexec-pipe)
      perror("pipe failed");
      continue;
    }

    pid_t exec_pid = fork();
    if (exec_pid < 0) {
      perror("fork failed");
      close(out_pipe[0]);
      close(out_pipe[1]);
      continue;
    }

    if (exec_pid == 0) {
      close(out_pipe[0]);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(out_pipe[1], STDERR_FILENO);
      close(out_pipe[1]);

      execute_command(cmd_buf);
    } else {
      close(out_pipe[1]);

      char read_buf[MAX_BUFFER];
      ssize_t rn;
      while ((rn = read(out_pipe[0], read_buf, MAX_BUFFER)) > 0) {
        if (write(client_fd, read_buf, (size_t)rn) < 0) {
          break;
        }
      }
      close(out_pipe[0]);
      waitpid(exec_pid, NULL, 0);

      char eot = (char)EOT_MARKER;
      if (write(client_fd, &eot, 1) < 0) {
        break;
      }
    }
  }
  close(client_fd);
}

// cppcheck-suppress constParameter
int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  uint16_t port = (uint16_t)strtol(argv[1], NULL, BASE_10);
  int server_fd = setup_server(port);

  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }

  printf("Server listening on port %d\n", port);

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t client_len = (socklen_t)sizeof(client_addr);

    memset(&client_addr, 0, sizeof(client_addr));
    int client_fd =
        accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept failed");
      continue;
    }

    printf("Accepted connection from %s:%d\n", inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork failed");
      close(client_fd);
    } else if (pid == 0) {
      close(server_fd);
      process_client(client_fd);
      printf("Client disconnected.\n");
      exit(EXIT_SUCCESS);
    } else {
      close(client_fd);
    }
  }

  close(server_fd);
  return EXIT_SUCCESS;
}
