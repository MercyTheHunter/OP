#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>

//CONSTANTS
#define NUM_ROUNDS 20
#define NUM_PLAYERS 2
#define LOG_SEM_NAME "krikett.log"


/////////////////////////////////////////////
// LOGGING
/////////////////////////////////////////////

sem_t *log_sem; //Semaphore for logging
FILE *log_file; //Logging file
char log_buffer[200]; //Logging buffer

void print_log(const char *format, ...) //(... needed for va_start)
{
    va_list args;
    va_start(args, format);

    //Logging to log file and stdout
    sem_wait(log_sem);
    vsprintf(log_buffer, format, args); //Print log message to buffer
    fprintf(log_file, "%s\n", log_buffer); //Write the log buffer into the log file
    fflush(log_file);
    printf("%s\n", log_buffer); //Print log to stdout
    fflush(stdout);
    sem_post(log_sem);

    va_end(args);
}

void create_logger()
{
    //Create the logging semaphore
    log_sem = sem_open(LOG_SEM_NAME, O_CREAT, S_IRUSR | S_IWUSR, 1);
    if (log_sem == SEM_FAILED)
    {
        perror("COULD NOT CREATE THE SEMAPHORE FOR LOGGING!");
        exit(EXIT_FAILURE);
    }

    //Create the log file
    log_file = fopen("log.txt", "w");
    if (log_file == NULL)
    {
        perror("COULD NOT CREATE LOG FILE!");
        exit(EXIT_FAILURE);
    }
    
    //Print message indicating the loggers creation
    print_log("Logger initialized!");
}

void close_logger()
{
    print_log("Closing logger...");
    sem_close(log_sem);
    sem_unlink(LOG_SEM_NAME);
    fclose(log_file);
}

/////////////////////////////////////////////
// SEMAPHORES
/////////////////////////////////////////////

//Player semaphores
sem_t *player_sems[NUM_PLAYERS];
char player_sem_name[NUM_PLAYERS][100];
//Game master semaphores
sem_t *gm_sems[NUM_PLAYERS];
char gm_sem_name[NUM_PLAYERS][100];

void create_semaphores()
{
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        sprintf(player_sem_name[i], "krikett.player_%d", i);
        player_sems[i] = sem_open(player_sem_name[i], O_CREAT, S_IRUSR | S_IWUSR, 0);
        sprintf(gm_sem_name[i], "krikett.gm.player_%d", i);
        gm_sems[i] =  sem_open(gm_sem_name[i], O_CREAT, S_IRUSR | S_IWUSR, 0);
    }
}

void close_semaphores()
{
    print_log("Closing semaphores...");
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        sem_close(player_sems[i]);
        sem_unlink(player_sem_name[i]);
        sem_close(gm_sems[i]);
        sem_unlink(gm_sem_name[i]);
    }
    
}

/////////////////////////////////////////////
// THE PIPE
/////////////////////////////////////////////

int in;
int out;

void create_channel()
{
    int pipe_handles[2];
    if (pipe(pipe_handles) == -1)
    {
        perror("COULD NOT CREATE PIPE!");
        exit(EXIT_FAILURE);
    }
    in = pipe_handles[0];
    out = pipe_handles[1];
}

void write_int(int x)
{
    write(out, &x, sizeof(x));
}

int read_int()
{
    int r;
    read(in, &r, sizeof(r));
    return r;
}

/////////////////////////////////////////////
// THE SIMULATION
/////////////////////////////////////////////

pid_t player_pids[NUM_PLAYERS];
int game_finished; //boolean
int hits[NUM_PLAYERS][22]; //1..20 + the bull (For some reason it's bugged as a [NUM_PLAYERS][21])
int scores[NUM_PLAYERS];

void signal_handler(int signal) {}

void sim_player(int player_index)
{
    //Setup
    srand(time(NULL) ^ getpid());
    signal(SIGUSR1, signal_handler);
    //Print log
    print_log("PLAYER%d: created", player_index+1);
    print_log("PLAYER%d: waiting to start", player_index+1);
    pause();
    print_log("PLAYER%d: started", player_index+1);

    while(1)
    {
        //Wait for round start
        sem_wait(gm_sems[player_index]);
        //Wait a random time, then throw
        sleep(1 + rand() % (3 - 1 + 1)); //1 to 3 seconds
        int sector = 13 + rand() % (21 - 13 + 1); //random number between 13 and 21 (The throw)
        int multiplier = 1 + rand() % (3 - 1 + 1); //random number between 1 and 3 (The multiplier of the throw: Regular, Double, Triple)
        //Send signal that round has finished
        sem_post(player_sems[player_index]);
        //Wait for the gm and send the result
        sem_wait(gm_sems[player_index]);
        write_int(sector);
        write_int(multiplier);
    }

    exit(EXIT_SUCCESS);
}

void calculate_score(int player, int sector, int multiplier)
{
    //Print log
    switch (multiplier)
    {
    case 2:
        print_log("GAME MASTER: PLAYER%d has hit a Double %d", player+1, sector);
        break;
    case 3:
        print_log("GAME MASTER: PLAYER%d has hit a Triple %d", player+1, sector);
        break;
    default: //1
        print_log("GAME MASTER: PLAYER%d has hit a Regular %d", player+1, sector);
        break;
    }
    

    //Update hits
    hits[player][sector] += multiplier;

    //Calculate score
    int score = 0;
    if (sector >= 15 && sector <= 20) //Scoring sectors
    {
        score = sector * multiplier;
    }
    else if (sector == 21) //Bull
    {
        if (multiplier == 3)
        {
            score = 50;
        }
        else
        {
            score = 25;
        }
    }

    //Update scores
    if (hits[player][sector] <= 3)
    {
        scores[player] += score;
    }
    
    //Check early finish
    int finished = 1;
    for (int i = 15; i < 22; ++i) //i in [15,21]
    {
        if (hits[player][i] < 3)
        {
            finished = 0;
            break;
        }
    }
    if (finished == 1)
    {
        game_finished = 1;
    }
}

void new_game()
{
    game_finished = 0;
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        scores[i] = 0;
        for (int j = 15; j < 22; ++j) //j in [15,21]
        {
            hits[i][j] = 0;
        }
    }
}

void print_results()
{
    print_log("\nGAME MASTER: game finished. The results:");
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        print_log("PLAYER%d", i+1);
        print_log("--Score = %d", scores[i]);
        print_log("--Sector hits (sector, hits):");
        for (int j = 15; j < 22; ++j) //j in [15,21]
        {
            print_log("    %d, %d", j, hits[i][j]);
        }
    }
}

void sim_gm()
{
    //Print log
    print_log("GAME MASTER: created");

    //Reset / create new game
    new_game();
    sleep(1);

    print_log("GAME MASTER: signaling players to start");

    //Setting up game (Create and signal players)
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        kill(player_pids[i], SIGUSR1);
    }
    
    //SIMULATION
    int round = 1;
    while (game_finished == 0)
    {
        sleep(1);

        //print round number
        print_log("\nGAME MASTER: Round %d started", round);

        //send round start signal to players
        for (int i = 0; i < NUM_PLAYERS; ++i)
        {
            sem_post(gm_sems[i]);
        }
        
        //wait for orund end signal from players
        for (int i = 0; i < NUM_PLAYERS; ++i)
        {
            sem_wait(player_sems[i]);
        }
        
        //collect results from players
        for (int i = 0; i < NUM_PLAYERS; ++i)
        {
            sem_post(gm_sems[i]);
            int sector = read_int();
            int multiplier = read_int();
            calculate_score(i, sector, multiplier);
        }
        
        //increase round number
        round += 1;

        //check if (round > NUM_ROUNDS)
        if (round > NUM_ROUNDS)
        {
            game_finished = 1;
        }
    }

    //Print results
    print_results();
    sleep(1);
}

void fork_players()
{
    for (int i = 0; i < NUM_PLAYERS; ++i)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("CANNOT CREATE CHILD PROCESS!");
            exit(EXIT_FAILURE);
        }
        else if (pid == 0)
        {
            //The child process
            sim_player(i);
        }
        else
        {
            //The parent process
            player_pids[i] = pid;
        }
    }
}

void main(int argc, char const *argv[])
{
    create_logger();
    create_semaphores();
    create_channel();

    fork_players();
    sim_gm();

    close_semaphores();
    close_logger();
}