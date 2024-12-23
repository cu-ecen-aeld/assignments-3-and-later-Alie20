#include "systemcalls.h"

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

/*
 * TODO  add your code here
 *  Call the system() function with the command set in the cmd
 *   and return a boolean true if the system() call completed with success
 *   or false() if it returned a failure
*/
	int ret_code = system(cmd);
	if (ret_code == -1 )
	{
		return false;
	}
    return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed
    command[count] = command[count];

/*
 * TODO:
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/

    va_end(args);
    
    //Fork A child process
    pid_t pid = fork(); 

	//Check the fork status
    if (pid < 0) 
    {
        perror("fork");
        return false;
    }
    else if (pid == 0) 
    {
        // Child process
        execv(command[0], command);

        // If execv returns, an error occurred
        perror("execv");
        exit(EXIT_FAILURE); // Exit child process with failure
    } 
    else 
    {
        // Parent process
        int status;
        if (waitpid(pid, &status, 0) == -1) 
        {
            perror("waitpid");
            return false;
        }

        // Check if the child terminated successfully
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) 
        {
            return true;
        } 
        else 
        {
            fprintf(stderr, "Command failed with exit status: %d\n", WEXITSTATUS(status));
            return false;
        }
    }

}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char *command[count + 1];
    int i;
    
    // Populate the command array
    for (i = 0; i < count; i++) {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    // Open the output file
    int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        perror("open");
        va_end(args);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        // Fork failed
        perror("fork");
        close(fd);
        va_end(args);
        return false;
    }

    if (pid == 0) {
        // Child process
        // Redirect stdout to the file
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2");
            close(fd);
            _exit(1);
        }

        // Close the file descriptor in the child process
        close(fd);

        // Execute the command
        execv(command[0], command);
        // If execv fails, exit the child process
        perror("execv");
        _exit(1);
    } else {
        // Parent process
        int status;
        close(fd); // Close the file descriptor in the parent process

        // Wait for the child process to complete
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            va_end(args);
            return false;
        }

        // Check if the child process exited successfully
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            va_end(args);
            return true;
        } else {
            va_end(args);
            return false;
        }
    }
}
