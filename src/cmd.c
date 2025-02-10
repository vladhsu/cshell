// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cmd.h"
#include "utils.h"

#define READ 0
#define WRITE 1
/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	if (!dir)
		return true;
	if (dir->next_word)
		return false;
	int res = chdir(dir->string);

	if (res == 0)
		return true;
	return false;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void) { return SHELL_EXIT; }

void solve_redirection(simple_command_t *s)
{
	int append_or_trunc_out;

	if (s->io_flags & IO_OUT_APPEND)
		append_or_trunc_out = O_APPEND;
	else
		append_or_trunc_out = O_TRUNC;

	int append_or_trunc_err;

	if (s->io_flags & IO_ERR_APPEND)
		append_or_trunc_err = O_APPEND;
	else
		append_or_trunc_err = O_TRUNC;

	if (s->in && s->in->string) {
		int fd = open(s->in->string, O_RDONLY);

		if (fd == -1)
			DIE(1, "in fd");
		dup2(fd, STDIN_FILENO);
		close(fd);
	}

	if (s->out && s->out->string) {
		int fd = open(s->out->string, O_CREAT | O_WRONLY | append_or_trunc_out,
					  0644);

		if (fd == -1)
			DIE(1, "out fd");
		dup2(fd, STDOUT_FILENO);
		close(fd);
	}

	if (s->err && s->err->string) {
		int fd = open(s->err->string, O_CREAT | O_WRONLY | append_or_trunc_err,
					  0644);

		if (fd == -1)
			DIE(1, "err fd");
		dup2(fd, STDERR_FILENO);
		close(fd);
	}

	if (s->out && s->err && strcmp(s->out->string, s->err->string) == 0)
		dup2(STDOUT_FILENO, STDERR_FILENO);
}

void restore_redirection(int saved_stdin, int saved_stdout, int saved_stderr)
{
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
	dup2(saved_stdout, STDOUT_FILENO);
	close(saved_stdout);
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	if (!s || !s->verb->string)
		return -1;

	char *cmd = get_word(s->verb);
	int saved_stdin = dup(STDIN_FILENO);
	int saved_stdout = dup(STDOUT_FILENO);
	int saved_stderr = dup(STDERR_FILENO);

	if (strcmp(cmd, "cd") == 0) {
		solve_redirection(s);
		free(cmd);
		bool res = shell_cd(s->params);

		restore_redirection(saved_stdin, saved_stdout, saved_stderr);
		return res ? 0 : 1;
	}
	if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
		free(cmd);
		return shell_exit();
	}
	if (strchr(cmd, '=')) {
		char *copy_cmd = strdup(cmd);

		if (copy_cmd != NULL) {
			char *save;
			char *name = strtok_r(copy_cmd, "=", &save);
			char *value = strtok_r(NULL, "=", &save);

			if (name && value) {
				int result = setenv(name, value, 1);

				free(copy_cmd);
				return result;
			}
			free(copy_cmd);
		}
		return -1;
	}
	free(cmd);

	pid_t pid, wait_ret;
	int status;

	pid = fork();
	if (pid == -1)
		DIE(1, "fork");
	if (pid == 0) {
		solve_redirection(s);
		word_t *params = s->params;
		int count = 0;

		while (params) {
			count++;
			params = params->next_word;
		}

		char **commmand = malloc((count + 2) * sizeof(char *));

		commmand[0] = strdup(s->verb->string);
		params = s->params;
		for (size_t i = 1; i <= count; i++) {
			commmand[i] = strdup(get_word(params));
			params = params->next_word;
		}
		commmand[count + 1] = NULL;
		int res = execvp(commmand[0], commmand);

		if (res == -1) {
			fprintf(stderr, "Execution failed for '%s'\n", commmand[0]);
			exit(EXIT_FAILURE);
		}
		DIE(1, "execvp");
	}

	wait_ret = waitpid(pid, &status, 0);
	DIE(wait_ret < 0, "waitpid");
	return WEXITSTATUS(status);
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
							command_t *father)
{
	int status_cmd1, status_cmd2;
	pid_t pid_cmd1, pid_cmd2;

	pid_cmd1 = fork();
	if (pid_cmd1 == -1)
		return false;
	if (pid_cmd1 == 0)
		exit(parse_command(cmd1, level + 1, father));

	pid_cmd2 = fork();
	if (pid_cmd2 == -1)
		return false;
	if (pid_cmd2 == 0)
		exit(parse_command(cmd2, level + 1, father));

	waitpid(pid_cmd1, &status_cmd1, 0);
	waitpid(pid_cmd2, &status_cmd2, 0);

	return (WEXITSTATUS(status_cmd2) == 0 && WEXITSTATUS(status_cmd1) == 0);
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
						command_t *father)
{
	int pipe_des[2];
	int rc;
	int status_cmd1, status_cmd2;
	pid_t pid_cmd1, pid_cmd2;

	rc = pipe(pipe_des);
	DIE(rc < 0, "pipe");
	pid_cmd1 = fork();
	if (pid_cmd1 == -1) {
		rc = close(pipe_des[READ]);
		DIE(rc < 0, "close");
		rc = close(pipe_des[WRITE]);
		DIE(rc < 0, "close");
		DIE(pid_cmd1, "fork");
		return EXIT_FAILURE;
	}
	if (pid_cmd1 == 0) {
		close(pipe_des[READ]);
		dup2(pipe_des[WRITE], STDOUT_FILENO);
		close(pipe_des[WRITE]);
		exit(parse_command(cmd1, level + 1, father));
	}
	pid_cmd2 = fork();
	if (pid_cmd2 == -1) {
		rc = close(pipe_des[READ]);
		DIE(rc < 0, "close");
		rc = close(pipe_des[WRITE]);
		DIE(rc < 0, "close");
		DIE(pid_cmd2, "fork");
		return EXIT_FAILURE;
	}
	if (pid_cmd2 == 0) {
		close(pipe_des[WRITE]);
		dup2(pipe_des[READ], STDIN_FILENO);
		close(pipe_des[READ]);
		exit(parse_command(cmd2, level + 1, father));
	}

	close(pipe_des[READ]);
	close(pipe_des[WRITE]);
	waitpid(pid_cmd1, &status_cmd1, 0);
	waitpid(pid_cmd2, &status_cmd2, 0);

	return (WEXITSTATUS(status_cmd1) == 0 && WEXITSTATUS(status_cmd2) == 0);
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	if (!c)
		return 0;

	if (c->op == OP_NONE)
		return parse_simple(c->scmd, level, father);
	switch (c->op) {
	case OP_SEQUENTIAL:
		parse_command(c->cmd1, level + 1, c);
		return parse_command(c->cmd2, level + 1, c);

	case OP_PARALLEL:
		return run_in_parallel(c->cmd1, c->cmd2, level, c);

	case OP_CONDITIONAL_NZERO: {
		if (parse_command(c->cmd1, level + 1, c) != 0)
			return parse_command(c->cmd2, level + 1, c);
		break;
	}

	case OP_CONDITIONAL_ZERO: {
		if (parse_command(c->cmd1, level + 1, c) == 0)
			return parse_command(c->cmd2, level + 1, c);
		break;
	}

	case OP_PIPE:
		return run_on_pipe(c->cmd1, c->cmd2, level, c);
	default:
		return SHELL_EXIT;
	}
	return 0;
}
