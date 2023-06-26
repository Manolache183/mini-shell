// SPDX-License-Identifier: BSD-3-Clause

#include "cmd.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "utils.h"

#define READ		0
#define WRITE		1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	if (dir == NULL)
		return true;

	char *word = get_word(dir);

	int r = chdir(word);

	if (r < 0) {
		char path[1024];
		char *ret = getcwd(path, sizeof(path));

		if (ret != NULL) {
			strcat(path, "/");
			strcat(path, word);

			r = chdir(path);
			if (r < 0) {
				printf("Error changing directory.\n");
				free(word);
				return false;
			}
		} else {
			printf("Error getting current directory.\n");
			free(word);
			return false;
		}
	}

	free(word);
	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	return SHELL_EXIT;
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	/* Sanity checks */

	if (s == NULL)
		return 1;

	/* Built-in commands */

	char *word = get_word(s->verb);

	if (strcmp(word, "cd") == 0) {
		char *file;
		int fd;

		if (s->in != NULL) {
			file = get_word(s->in);
			fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				free(file);
				printf("Open error\n");
				return 1;
			}
			free(file);
			close(fd);
		}

		if (s->out != NULL) {
			file = get_word(s->out);
			fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				free(file);
				printf("Open error\n");
				return 1;
			}
			free(file);
			close(fd);
		}

		if (s->err != NULL) {
			file = get_word(s->err);
			fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				free(file);
				printf("Open error\n");
				return 1;
			}
			free(file);
			close(fd);
		}

		free(word);
		if (shell_cd(s->params) == true)
			return 0;
		else
			return 1;
	} else if (strcmp(word, "exit") == 0 || strcmp(word, "quit") == 0) {
		free(word);
		return shell_exit();
	}

	/* Variable assignment */

	if (word != NULL && strchr(word, '=') != NULL) {
		char *var = strtok(word, "=");
		char *val = strtok(NULL, "=");

		if (var != NULL && val != NULL) {
			int ret = setenv(var, val, 1);

			free(word);
			return ret;
		}

		free(word);
		return 1;
	}

	/* External command */

	pid_t pid = fork();

	if (pid < 0) {
		printf("fork\n");
		return 1;
	} else if (pid == 0) {
		/* Child */

		int fdin = -1, fdout = -1, fderr = -1;

		if (s->in != NULL) {
			char *input = get_word(s->in);

			fdin = open(input, O_RDONLY);
			free(input);

			if (fdin < 0) {
				free(word);

				printf("Open error\n");
				return 1;
			}

			if (dup2(fdin, STDIN_FILENO) < 0) {
				close(fdin);
				free(word);

				printf("dup2 error\n");
				return 1;
			}
		}

		if (s->out != NULL) {
			char *output = get_word(s->out);

			if (s->io_flags > 0)
				fdout = open(output, O_WRONLY | O_CREAT | O_APPEND, 0644);
			else
				fdout = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			free(output);

			if (fdout < 0) {
				free(word);

				printf("Open error\n");
				return 1;
			}

			if (dup2(fdout, STDOUT_FILENO) < 0) {
				free(word);
				close(fdout);

				printf("dup2 error\n");
				return 1;
			}
		}

		if (s->err != NULL) {
			char *error = get_word(s->err);
			char *output = get_word(s->out);

			if (fdout >= 0 && strcmp(output, error) == 0) {
				fderr = fdout;
			} else {
				if (s->io_flags > 0)
					fderr = open(error, O_WRONLY | O_CREAT | O_APPEND, 0644);
				else
					fderr = open(error, O_WRONLY | O_CREAT | O_TRUNC, 0644);

				if (fderr < 0) {
					free(error);
					free(word);
					free(output);

					printf("Open error\n");
					return 1;
				}
			}

			if (dup2(fderr, STDERR_FILENO) < 0) {
				free(error);
				free(word);
				free(output);
				close(fderr);

				printf("dup2 error\n");
				return 1;
			}

			free(error);
			free(output);
		}

		int num_args = 0;
		char * const *argv = get_argv(s, &num_args);
		int r = execvp(word, argv);

		if (r < 0) {
			printf("Execution failed for '%s'\n", word);
			free(word);
			exit(r);
		}

		return r;

	} else {
		/* Parent */

		free(word);
		int status;

		if (waitpid(pid, &status, 0) < 0) {
			printf("waitpid error\n");
			return 1;
		}

		if (WIFEXITED(status))
			return WEXITSTATUS(status);

		printf("Child process did not terminate normally\n");
		return 1;
	}

	return 0;
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	pid_t pid_left = fork();

	if (pid_left < 0) {
		printf("Probles with fork");
		return false;
	} else if (pid_left == 0) {
		/* Child */
		int status = parse_command(cmd1, level + 1, father);

		exit(status);
	} else {
		/* Parent */
		pid_t pid_right = fork();

		if (pid_right < 0) {
			printf("Probles with fork");
			return false;
		} else if (pid_right == 0) {
			/* Child */
			int status = parse_command(cmd2, level + 1, father);

			exit(status);
		} else {
			/* Parent */
			int status;

			if (waitpid(pid_left, &status, 0) < 0) {
				printf("waitpid error\n");
				return false;
			}

			if (waitpid(pid_right, &status, 0) < 0) {
				printf("waitpid error\n");
				return false;
			}

			return true;
		}
	}

	return true;
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	int fd[2];
	int r = pipe(fd);

	if (r < 0) {
		printf("Pipe error");
		return false;
	}

	pid_t pid_left = fork();

	if (pid_left < 0) {
		printf("Probles with fork");
		return false;
	} else if (pid_left == 0) {
		/* Child */
		close(fd[READ]);

		if (dup2(fd[WRITE], STDOUT_FILENO) < 0) {
			close(fd[WRITE]);
			printf("dup2 error\n");
			return false;
		}

		int r = parse_command(cmd1, level + 1, father);

		exit(r);
	} else {
		/* Parent */
		pid_t pid_right = fork();

		if (pid_right < 0) {
			printf("Probles with fork");
			return false;
		} else if (pid_right == 0) {
			/* Child */

			close(fd[WRITE]);
			if (dup2(fd[READ], STDIN_FILENO) < 0) {
				close(fd[READ]);
				printf("dup2 error\n");
				return false;
			}

			int r = parse_command(cmd2, level + 1, father);

			exit(r);
		} else {
			/* Parent */
			close(fd[READ]);
			close(fd[WRITE]);

			int status;

			if (waitpid(pid_left, &status, 0) < 0) {
				printf("waitpid error\n");
				return false;
			}

			if (waitpid(pid_right, &status, 0) < 0) {
				printf("waitpid error\n");
				return false;
			}

			if (status != 0)
				return false;

			return true;
		}
	}
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* Sanity checks */
	if (c == NULL)
		return SHELL_EXIT;

	int r = 0;

	if (c->op == OP_NONE) {
		/* Execute a simple command. */
		r = parse_simple(c->scmd, level + 1, c);
		return r;
	}

	switch (c->op) {
	case OP_SEQUENTIAL:
		/* Execute the commands one after the other. */
		r = parse_command(c->cmd1, level + 1, c);
		r = parse_command(c->cmd2, level + 1, c);
		break;

	case OP_PARALLEL:
		/* Execute the commands simultaneously. */
		if (run_in_parallel(c->cmd1, c->cmd2, level, c) == false)
			r = 1;
		else
			r = 0;

		break;

	case OP_CONDITIONAL_NZERO:
		/* Execute the second command only if the first one returns non zero. */
		r = parse_command(c->cmd1, level + 1, c);
		if (r != 0)
			r = parse_command(c->cmd2, level + 1, c);

		break;

	case OP_CONDITIONAL_ZERO:
		/* Execute the second command only if the first one returns zero. */
		r = parse_command(c->cmd1, level + 1, c);
		if (r == 0)
			r = parse_command(c->cmd2, level + 1, c);

		break;

	case OP_PIPE:
		/* Redirect the output of the first command to the input of the second. */
		if (run_on_pipe(c->cmd1, c->cmd2, level, c) == false)
			r = 1;
		else
			r = 0;
		break;

	default:
		return SHELL_EXIT;
	}

	return r;
}
