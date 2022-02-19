#define _GNU_SOURCE
#include <fcntl.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#define PIPE "|"
#define REDIRECT_IN "<"
#define REDIRECT_OUT ">"
#define REDIRECT_ERR "2>"


enum Pipe_Side { LEFT = 0, RIGHT = 1, NO_PIPE = -1 };
enum Status { NONE = -2, STOPPED = -1, RUNNING = 0, DONE = 1 };


typedef struct process {
	char* command;
	char** argv;
	int numTokens;
	int pgid;
	int redirectInIndex;	// Each process should have at most one of each type of redirection
	int redirectOutIndex;
	int redirectErrIndex;
	char* stdInFile;
	char* stdOutFile;
	char* stdErrFile;
	int pipeIndex;
	enum Pipe_Side pipeSide;
} process;


typedef struct job {
	int jobId;
	char* jobString;
	int pgid;
	int status;
	bool isBg;
} job;


// Stopped and background jobs will go onto the stack
// Used to keep track of the order of the processes in which fg/bg commands will apply to
typedef struct stack {
	int top;	// Refers to actual indices of jobStack
	int capacity;
	int jobStack[20];		// Every element in jobStack will be a job ID
} stack;

job* jobsList[20];		// Maximum # of jobs will be 20
int mostRecentJob = 0;		// Mainly used for 'jobs' printing purposes
int mostRecentFgJob = 0;	// Used to track which job will receive signals

int top = -1;
int jobStack[20];



int GetInputTokens(char** input, char** inputTokenPointer[]);
process* createProcess(char** input, char* tokenPointer[], int numTokens, enum Pipe_Side side, int pipeIndex);
int run(process* currProcess, int pipefds[], int pgid);
void initJob(job* jobPointer);
void manageJobs();
void printZombies();
void printJob(bool mostRecent, int jobId, int status, char* jobString);
job* getNextJob(job* jobsList[]);
int getNextJobIndex();
int getRecentlyStopped();
int getMostRecentJob();
void sigHandler(int signalName);



int main(void) {
	
	// Set up array of empty jobs
	for (int i = 0; i < 20; i++) {
		jobsList[i] = (job*)malloc(sizeof(job));
		initJob(jobsList[i]);
		jobStack[i] = 0;
	}

    int pipefds[2];
    int status;

    signal(SIGCHLD, sigHandler);
  	signal(SIGTSTP, sigHandler);
  	signal(SIGINT, sigHandler);


    while (1) {
    	signal(SIGTTOU, SIG_IGN);
    	signal(SIGCHLD, sigHandler);
    	signal(SIGTSTP, sigHandler);
    	
    	process* pipeLeftProcess = NULL;
    	process* pipeRightProcess = NULL;
    	int cpidLeft;
    	int cpidRight;
    	bool bgJob = false;

    	char* inputJobString;
		char** inputTokens;
    	char* input = readline("# ");
    	
    	if (input != NULL) {
    		inputJobString = strdup(input);		// Points to a duplicate of the input string, use as ->jobString later
    	}
    	// printf("input is: %s\n", input);
    	// Ctrl+D generates EOF ==> should exit shell
    	if (input == NULL) {
    		printf("\n");
    		// printf("Input is null, exiting the shell\n\n");
    		break;
    	}
    	
    	int numTokens = GetInputTokens(&input, &inputTokens);   
    	// printf("The number of tokens is: %d\n", numTokens); 

    	if (*inputTokens[numTokens-1] == '&') {
    		bgJob = true;
    		inputTokens[numTokens-1] = NULL;	// Remove "&" from the input
    		numTokens--;
    	}

    	if (strcmp(inputTokens[0], "fg") == 0) {
    		// Need to signal the most recently backgrounded/stopped process to continue
    		if (top == -1) {
    			printf("fg: current: no such job\n");
    		}
    		else {
	    		printf("fg command triggered, job at the top of the stack is %d\n", jobStack[top]);
	    		int topJobId = jobStack[top];
	    		job* recentJob = jobsList[topJobId-1];
	    		kill(-(recentJob->pgid), SIGCONT);	
	    		kill(recentJob->pgid, SIGCONT);
	    		recentJob->status = RUNNING;
	    		printf("%s\n", recentJob->jobString);

	    		waitpid(-(recentJob->pgid), &status, WUNTRACED);
	    		mostRecentFgJob = recentJob->jobId;
	    	}

	    	continue;
    	}


    	if (strcmp(inputTokens[0], "bg") == 0) {
    		if (top == -1) {
    			printf("bg: current: no such job\n");
    		}
    		else {
    			// Signal the most recently stopped process to continue in the background
    			int stoppedId = getRecentlyStopped();
    			job* stoppedJob = jobsList[stoppedId];

    			kill(-(stoppedJob->pgid), SIGCONT);	
	    		kill(stoppedJob->pgid, SIGCONT);

	    		stoppedJob->status = RUNNING;
	    		printf("%s\n", stoppedJob->jobString);

	    		// Stack manipulation performed in sigHandler when the job finishes
	    		
	    		// free(stoppedJob);
    		}

	    	// Background jobs don't respond to CTRL+Z because it is not currently controlling the shell
	    	// Let background job run
	    	waitpid(-cpidLeft, &status, WNOHANG | WUNTRACED);

    		continue;
    	}

   
    	if (strcmp(inputTokens[0], "jobs") == 0) {
   			manageJobs();
   			continue;
    	}

    	// Print any processes that have finished
    	printZombies();

    	// Process creation
    	process* currProcess = createProcess(&input, inputTokens, numTokens, NO_PIPE, -1);
    	if (currProcess->pipeIndex != -1) {
    		pipeRightProcess = createProcess(&input, inputTokens, numTokens, RIGHT, currProcess->pipeIndex);
    		pipe(pipefds);
    	}


    	cpidLeft = run(currProcess, pipefds, -1);
    	setpgid(cpidLeft, 0);	// Create process group with pgid = cpidLeft
    	// printf("The pgid of the child is: %d\n", getpgid(cpidLeft));
    	if (currProcess->pipeIndex != -1) {
    		cpidRight = run(pipeRightProcess, pipefds, cpidLeft);
    		setpgid(cpidRight, getpgid(cpidLeft));
    	}

    	int nextIndex = getNextJobIndex();
    	job* newJob = jobsList[nextIndex];
    	newJob->jobId = nextIndex+1;
    	newJob->status = RUNNING;
    	newJob->pgid = cpidLeft;
    	newJob->jobString = inputJobString;
    	newJob->isBg = bgJob;		// Should have vetted for this earlier
    	mostRecentJob = newJob->jobId;

    	close(pipefds[0]);
    	close(pipefds[1]);

    	if (bgJob == true) {
    		top++;
    		jobStack[top] = newJob->jobId;
    		waitpid(-cpidLeft, &status, WNOHANG | WUNTRACED);
    	}
    	else {
    		mostRecentFgJob = newJob->jobId;
    		// printf("mostRecentFgJob should have been set, value is: %d\n", mostRecentFgJob);
    		waitpid(-cpidLeft, &status, WUNTRACED);
    	}
    	// printf("\n\n");
    	free(currProcess->argv);
    	// free(currProcess);
    	free(inputTokens);
    	free(input);
    	// close(pipefds[0]);
    	// close(pipefds[1]);
	}

	// for (int i = 0; i < 20; i++) {
	// 	free(jobsList[i]);
	// }

	return 0;
}


int GetInputTokens(char** input, char** inputTokenPointer[]) {
	char** inputTokens = (char **)malloc(sizeof(char *) * 2000);	// Input is 2000 characters max

	int index = 0;
	char* token = strtok(*input, " ");
	while (token != NULL) {
		inputTokens[index] = token;
		token = strtok(NULL, " ");
		index++;
	}

	inputTokens[index] = NULL;	// Ensure last element after input tokens is NULL
	*inputTokenPointer = inputTokens;	// inputTokens in main now points to a populated char[]

	return index;
}


void initProcess(process* processPointer) {
	processPointer->command = NULL;
	processPointer->argv = NULL;
	processPointer->numTokens = -1;
	processPointer->pgid = -1;
	processPointer->redirectInIndex = -1;
	processPointer->redirectOutIndex = -1;
	processPointer->redirectErrIndex = -1;
	processPointer->stdInFile = NULL;
	processPointer->stdOutFile = NULL;
	processPointer->stdErrFile = NULL;
	processPointer->pipeIndex = -1;
	processPointer->pipeSide = NO_PIPE;
}


process* createProcess(char** input, char* tokenPointer[], int numTokens, enum Pipe_Side side, int pipeIndex) {
	process* newProcess = malloc(sizeof(process));
	initProcess(newProcess);
	// bool ampersand = false;

	char** argv = (char **)malloc(sizeof(char*) * 2000);
	newProcess->argv = argv;

	if (side == NO_PIPE) {
		// TODO: LOOK FOR "&" IN LEFT SIDE OF PIPE IF PIPE EXISTS, THIS IS INVALID BUT MUST BE HANDLED 

		for (int i = 0; i < numTokens; i++) {
			if (i == 0) {
				newProcess->command = tokenPointer[i];
			}

			// Check for redirection
			if (strcmp(tokenPointer[i], REDIRECT_IN) == 0) {
				newProcess->redirectInIndex = i+1;		// The next token after redirection should be the desired file/command	
				newProcess->stdInFile = tokenPointer[i+1];
			}

			else if (strcmp(tokenPointer[i], REDIRECT_OUT) == 0) {
				newProcess->redirectOutIndex = i+1;
				newProcess->stdOutFile = tokenPointer[i+1];
			}

			else if (strcmp(tokenPointer[i], REDIRECT_ERR) == 0) {
				newProcess->redirectErrIndex = i+1;
				newProcess->stdErrFile = tokenPointer[i+1];
			}

			// Check for a pipe
			else if (strcmp(tokenPointer[i], PIPE) == 0) {
				newProcess->pipeIndex = i;		// Store pipe index to process all arguments on right side of pipe later
				newProcess->pipeSide = LEFT;	// Update pipe side if the pipe exists
				newProcess->argv[i+1] = NULL;
				return newProcess;		// Processing the left side of the pipe is done
			}
			// Everything else goes into argv
			else {
				newProcess->argv[i] = tokenPointer[i];
			}
		}
		newProcess->argv[numTokens] = NULL;
	}

	else if (side == RIGHT) {
		int argvIndex = 0;
		int baseCommandIndex = pipeIndex + 1;	// Base command should be right after the pipe
		newProcess->command = tokenPointer[baseCommandIndex];	// Process command (e.g. ls, sleep, etc.) is the first argument of the input
		newProcess->pipeSide = RIGHT;
		for (int i = pipeIndex+1; i < numTokens; i++) {

			// Check for redirection
			if (strcmp(tokenPointer[i], REDIRECT_IN) == 0) {
				newProcess->redirectInIndex = i+1;	// The next token after redirection should be the desired file/command	
				newProcess->stdInFile = tokenPointer[i+1];
			}

			else if (strcmp(tokenPointer[i], REDIRECT_OUT) == 0) {
				newProcess->redirectOutIndex = i+1;
				newProcess->stdOutFile = tokenPointer[i+1];
			}

			else if (strcmp(tokenPointer[i], REDIRECT_ERR) == 0) {
				newProcess->redirectErrIndex = i+1;
				newProcess->stdErrFile = tokenPointer[i+1];
			}

			// Everything else goes into argv
			else {
				newProcess->argv[argvIndex] = tokenPointer[i];
				argvIndex++;
			}
			// printf("Token on the right side of the pipe: %s \n", tokenPointer[i]);
		}
		newProcess->argv[argvIndex] = NULL;
	}

	return newProcess;
}


int run(process* currProcess, int pipefds[], int pgid) {
	pid_t pid = fork();

	if (pid == 0) {
		if (pgid != -1) {
			int verifySetPgid = setpgid(0, pgid);	// Add to left process' group
		}
		if (pgid == -1) {
			setpgid(0, 0);
		}

		// Set up pipe properly
		if (currProcess->pipeSide == LEFT) {
			close(pipefds[0]);		// Close read end of the pipe
			dup2(pipefds[1], 1);	
		}
		else if (currProcess->pipeSide == RIGHT) {
			close(pipefds[1]); 		// Close write end of the pipe
			dup2(pipefds[0], 0);	
		}

		// Set up file redirects
		if (currProcess->stdOutFile != NULL) {
			int outFileDescriptor = open(currProcess->stdOutFile, O_CREAT | O_WRONLY | O_TRUNC, 
											S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
			dup2(outFileDescriptor, 1);
		}

		if (currProcess->stdInFile != NULL) {
			int inFileDescriptor = open(currProcess->stdInFile, O_RDONLY);
			// File does not exist
			if (inFileDescriptor == -1) {
				exit(-1);
				return -1;
			}
			dup2(inFileDescriptor, 0);
		}

		if (currProcess->stdErrFile != NULL) {
			int errFileDescriptor = open(currProcess->stdErrFile, O_CREAT | O_WRONLY | O_TRUNC, 
											S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
			dup2(errFileDescriptor, 2);
		}
		// setpgid(0, 0);
		int exec_code = execvp(currProcess->command, currProcess->argv);
		if (exec_code == -1) {
			printf("Exec code is -1\n");
			exit(-1);
		}
	}
	else if (pid == -1) {
		printf("Fork failed\n");
	}
	
	return pid;		// Returns the child's pid
}


void initJob(job* jobPointer) {
	jobPointer->jobId = -1;
	jobPointer->jobString = NULL;
	jobPointer->pgid = -2;
	jobPointer->status = NONE;
	jobPointer->isBg = false;
}



void manageJobs() {
	bool isMostRecent = false;

    for (int i = 0; i < 20; i++) {
    	job* currJob = jobsList[i];
    	// printf("ID of the current job is: %d\n", jobsList[i]->jobId);
    	if (jobsList[i]->jobId == mostRecentJob) {
    		isMostRecent = true;
    	}

    	// Skip over empty jobs
    	if (jobsList[i]->status != NONE) {
    		printJob(isMostRecent, jobsList[i]->jobId, jobsList[i]->status, jobsList[i]->jobString);
    	}

    	// If a job is done, reset its pointer in jobsList
    	if (jobsList[i]->status == DONE) {
    		initJob(jobsList[i]);
    	}
    }

    // mostRecentJob = getMostRecentJob();
}

// If the user did not prompt 'jobs', print done status for any zombie children
void printZombies() {
	bool isMostRecent = false;

	for (int i = 0; i < 20; i++) {
		if (jobsList[i]->status == DONE) {
			if (jobsList[i]->jobId == mostRecentJob) {
				isMostRecent = true;
			}
			printJob(isMostRecent, jobsList[i]->jobId, jobsList[i]->status, jobsList[i]->jobString);
			initJob(jobsList[i]);
		}
	}

	mostRecentJob = getMostRecentJob();
}


void printJob(bool mostRecent, int jobId, int status, char* jobString) {
	char* printStatus;

	switch(status) {
		case (STOPPED):
			printStatus = "Stopped";
			break;

		case (RUNNING):
			printStatus = "Running";
			break;

		case (DONE):
			printStatus = "Done";
			break;
	}


	if (mostRecent == true) {
		printf("[%d]+\t%s \t\t\t%s\n", jobId, printStatus, jobString);
	}
	else {
		printf("[%d]-\t%s \t\t\t%s\n", jobId, printStatus, jobString);
	}
}


int getNextJobIndex() {
	int highestIndex = -1;

	for (int i = 0; i < 20; i++) {
		if (jobsList[i]->jobId > 0) {
			highestIndex = i;
		}
	}

	return highestIndex+1;		// ID of the newest job should be one greater than the current highest
}


job* getNextJob(job* jobsList[]) {
	int highestId = 0;

	for (int i = 0; i < 20; i++) {
		if (jobsList[i]->status != NONE) {
			highestId = i;
		}
	}

	if (highestId > 0){
		// jobsList[highestId+1]->jobId = highestId + 2;
		// jobsList[highestId+1]->status = RUNNING;
		return jobsList[highestId+1];
	}

	return jobsList[0];
}


int getRecentlyStopped() {
	int highestStopped = -1;

	for (int i = 19; i > -1; i--) {
		// if (jobsList[i]->status == STOPPED) {
		// 	highestStopped = i;
		// }
		int jobId = jobStack[i];
		if (jobsList[jobId]->status == STOPPED) {
			return i + 1;
		}
	}

	return 0;
	// return highestStopped+1;
}


int getMostRecentJob() {
	int mostRecent = -1;

	for (int i = 0; i < 20; i++) {
		if (jobsList[i]->status != NONE) {
			mostRecent = i;
		}
	}

	return mostRecent+1;	// Will return 0 if there are no active jobs at the time
}


void sigHandler(int signalName) {
	switch (signalName) {

		// Zombie children are children who have terminated but haven't been waited on
		// (i.e. background processes that have finished but haven't officially been acknowledged as being done)
		case SIGCHLD:
			// WHERE MY BABIES @
			for (int i = 0; i < 20; i++) {
				int pgid = waitpid(-(jobsList[i]->pgid), 0, WNOHANG);
				
				if (pgid == jobsList[i]->pgid) {
					mostRecentJob = getMostRecentJob();
					mostRecentFgJob = 0;
					// printf("A foreground job as finished, the job was %s\n", jobsList[i]->jobString);
					for (int j = 0; j < 20; j++) {
						if (jobStack[i] == jobsList[i]->jobId) {
							jobStack[i] = 0;
							if (i == top) {
								top--;
							}
						}
					}

					if (jobsList[i]->isBg) { 
						jobsList[i]->status = DONE;
					}

					else if (!jobsList[i]->isBg) {
						// printf("The job that just finished is: %s\n", jobsList[i]->jobString);
						initJob(jobsList[i]);
					}
				}
			}

			break;

		// CTRL+Z only applies to what is controlling the terminal
		case SIGTSTP:
			printf("The job id of the controlling job is: %d\n", mostRecentFgJob);
			if (mostRecentFgJob > 0) {
				kill(-(jobsList[mostRecentFgJob-1]->pgid), SIGTSTP);
				jobsList[mostRecentFgJob-1]->status = STOPPED;
				// waitpid(-(jobsList[mostRecentFgJob-1]->pgid), 0, WNOHANG | WUNTRACED);
				
				// Add the stopped job to the stack
				top++;
				jobStack[top] = jobsList[mostRecentFgJob-1]->jobId;
			}
			break;

		// CTRL+C only applies to what is controlling the terminal
		case SIGINT:
			if (mostRecentFgJob > 0) {
				kill(-(jobsList[mostRecentFgJob-1]->pgid), SIGINT);
				printf("\n");
				// waitpid(-(jobsList[mostRecentFgJob-1]->pgid), 0, WNOHANG | WUNTRACED);
			}
			break;
	}
}
















