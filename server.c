#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <pthread.h>
#include <sys/stat.h>
#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
             kill( 0, SIGKILL ),\
		     exit(EXIT_FAILURE))

#define BACKLOG 3
#define CLEAR( x ) memset( &(x), 0, sizeof( x ) )
#define WRITE_CMD( str ) { if ( bulk_write(c->s,str "\n",sizeof( str "\n" )) <= 0 ) c->do_work = 0; }

#define WAKE_CLIENT() { if ( kill(c->pid,SIGALRM) ) ERR("kill"); }
#define GET_SHARED_MEM( ptr, id ) (ptr) = shared_mem_get( id );

#define BOARD_N 8
#define BOARD_SIZE (BOARD_N*BOARD_N)
#define DATA_DIR "data/"


typedef enum { // client states
    client_waiting,
    client_playing,
    client_connected,
} cli_state_t;

typedef enum { // command to client
    client_game_started,
    client_game_exited,
    client_game_send_status,
} cli_cmd_t;

struct game_data { // game data structure
    int p[ 2 ];
    volatile sig_atomic_t do_work;
    
    char board[ BOARD_SIZE ];
    int moving_player;
    key_t key;
    int is_saved;

    int move_num;
};

struct cli_data { // client data structure
    int s;
    volatile sig_atomic_t do_work;

    pid_t pid;
    int move_pos;
    int move_tile;
    cli_state_t state;
    int cmd_from_game;

    int cli_mem_id;
    key_t game_key;
    int game_mem_id;
    int id;

    char cmd_buf[ 256 ];
    char buf[ 256 ];

    int score;
    int player_num; // 0 - 1
    struct cli_data *opponent_ptr;
    struct game_data *game_ptr;
};

typedef struct cli_data cli_data_t;
typedef struct game_data game_data_t;

void game_load_data( game_data_t *g, cli_data_t *c ) 
{
    char buf[256];
    sprintf(buf, DATA_DIR "%u_%u_data.txt", g->key, c->id);
    int fd = open( buf, O_RDONLY );
    if ( fd == -1 ) 
      return; // no saved game
    if ( TEMP_FAILURE_RETRY(read(fd,g->board,BOARD_SIZE))!=BOARD_SIZE ) 
      ERR("read");
    if ( TEMP_FAILURE_RETRY(close(fd)) ) 
      ERR("close");
}

void game_store_data( game_data_t *g, cli_data_t *c, cli_data_t *c2 ) 
{
    if ( g->is_saved )
      return; 
    g->is_saved = 1;
    char buf[256];
    sprintf(buf,DATA_DIR "%u_data.txt",g->key);
    int fd = open( buf, O_WRONLY|O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH );
    if ( fd == -1 ) 
      ERR( "open" );
    if ( TEMP_FAILURE_RETRY(write(fd,g->board,BOARD_SIZE))!=BOARD_SIZE ) 
      ERR("write");
    if ( TEMP_FAILURE_RETRY(close(fd)) ) 
      ERR("close");
    sprintf(buf,"%u_data.txt",g->key);
    char buf2[ 256 ];
    sprintf( buf2, DATA_DIR "%u_%u_data.txt", g->key, c->id );
    if (symlink( buf, buf2 ) && errno!=EEXIST) 
      ERR("symlink");;
    sprintf( buf2, DATA_DIR "%u_%u_data.txt", g->key, c2->id );
    if (symlink( buf, buf2 ) && errno!=EEXIST) 
      ERR("symlink");
}

void show_file( int out_fd, char *fname ) 
{ 
    char buf[256];
    int fd = open( fname, O_RDONLY );
    if ( fd == -1 ) ERR( "open" );
    int a = 0;
    do {
        if (TEMP_FAILURE_RETRY(write( out_fd, buf, a ))!=a) 
          return;
        a = TEMP_FAILURE_RETRY(read(fd,buf,sizeof(buf)));
    } while (a>0);

    if ( TEMP_FAILURE_RETRY(close(fd)) ) 
      ERR("close");
}

void file_append_line( char *fname, char *line ) {
    int fd = open( fname, O_WRONLY|O_APPEND|O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH );
    if ( fd == -1 ) 
      ERR( "open" );
    if (TEMP_FAILURE_RETRY(write(fd,line,strlen(line)))!=strlen(line)) 
      ERR("write");
    if (TEMP_FAILURE_RETRY(write(fd,"\n",1))!=1) 
      ERR("write");
    if ( TEMP_FAILURE_RETRY(close(fd)) ) 
      ERR("close");
}

void client_show_file( game_data_t *g, cli_data_t *c, char *file_suf ) {
    char buf[256];
    sprintf(buf,DATA_DIR "%u_%u_%s.txt",g->key,c->id,file_suf);
    show_file( c->s, buf );
}
void client_append_line( game_data_t *g, cli_data_t *c, char *file_suf, char *line ) {
    char buf[256];
    sprintf(buf,DATA_DIR "%u_%u_%s.txt",g->key,c->id,file_suf);
    file_append_line( buf, line );
}

void game_show_file( game_data_t *g, cli_data_t *c, char *file_suf ) {
    char buf[256];
    sprintf(buf,DATA_DIR "%d_%s.txt",g->key,file_suf);
    show_file( c->s, buf );
}
void game_append_line( game_data_t *g, char *file_suf, char *line ) {
    char buf[256];
    sprintf(buf,DATA_DIR "%d_%s.txt",g->key,file_suf);
    file_append_line( buf, line );
}

void client_send_msg( game_data_t *g, cli_data_t *c, cli_data_t *opponent_ptr, char *msg ) {
    client_append_line( g, opponent_ptr, "msg", msg );
    snprintf( opponent_ptr->cmd_buf, sizeof(opponent_ptr->cmd_buf), "%s\n", msg );
    if ( kill(opponent_ptr->pid,SIGALRM) ) 
      ERR("kill");
}

void sigint_handler(int sig) 
{
    ERR( "killed." );
}

void sigalrm_handler(int sig) { }

int sethandler( void (*f)(int), int sigNo) 
{
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1==sigaction(sigNo, &act, NULL))
		return -1;
	return 0;
}


int make_socket(int domain, int type) 
{
	int sock = socket(domain,type,0);
	if(sock < 0) 
    	ERR("socket");
	return sock;
}

int bind_inet_socket(uint16_t port,int type) 
{
    struct sockaddr_in addr;
    int socketfd = make_socket(PF_INET, type);
    int t = 1;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t))) 
    	ERR("setsockopt");
    if(bind(socketfd,(struct sockaddr*) &addr, sizeof(addr)) < 0) 
    	ERR("bind");
    if(SOCK_STREAM==type)
    	if(listen(socketfd, BACKLOG) < 0) 
    		ERR("listen");
    return socketfd;
}


int add_new_client(int sfd) 
{
  	int nfd;
  	if((nfd=TEMP_FAILURE_RETRY(accept(sfd,NULL,NULL)))<0) {
  		if(EAGAIN==errno||EWOULDBLOCK==errno) return -1;
  		ERR("accept");
  	}
  	return nfd;
}

void usage(char * name) {
	fprintf(stderr,"USAGE: %s port\n",name);
}

ssize_t bulk_write(int fd, char *buf, size_t count)
{
	int c;
	size_t len=0;
	do {
		c=TEMP_FAILURE_RETRY(write(fd,buf,count));
		if(c<0) 
      return c;
		buf+=c;
		len+=c;
		count-=c;
	} while(count>0);
	return len ;
}

int recv_line(int fd, char *buf, size_t buf_size) // receive a line from socket
{
    int l = 0;
    while ( l < buf_size  ) 
    {
        int a = recv( fd, buf+l, buf_size-l, 0 );
        if  ( a > 0 ) {
            l += a;

            if ( buf[l-1] == '\n' ) { 
            	buf[l-1] = 0; 
            	return 0; 
            }

            if ( l == buf_size ) 
            	return -1;
        
        } else 
            return -1;
    }
    return 0;
}

// create shared memory piece 
// return address of the attached shared memory segment
void * shared_mem_set( int *id, int sz )
{
    *id = shmget( IPC_PRIVATE, sz, IPC_CREAT | 0600 );
    if ( *id == -1 ) 
		ERR( "shmget" );
    void *ret = shmat( *id, NULL, 0 );
    if ( !ret ) 
    	ERR("shmat");
    return ret; 
}

void * shared_mem_set_const_key( key_t key, int *id, int sz )
{
    *id = shmget( key, sz, IPC_CREAT | 0600 );
    if ( *id == -1 ) 
    	ERR( "shmget" );
    void *ret = shmat( *id, NULL, 0 );
    if ( !ret ) 
    	ERR("shmat");
    return ret;
}

int shared_mem_get_id( key_t key, int sz )
{
    int id = shmget( key, sz, 0600 );
    if ( id == -1 ) 
    	ERR( "shmget" );
    return id;
}

// get shared memory piece
void * shared_mem_get( int id )
{
    void *ret = shmat( id, NULL, 0 );
    if ( !ret ) 
    	ERR("shmat");
    return ret;
}


void shared_mem_set_autodestroy( int id )
{
	//Mark the segment to be destroyed. 
	//The segment will only actually be destroyed after the last process detaches it
    if ( shmctl(id, IPC_RMID, NULL) ) 
    	ERR( "shmctl" );
}

#define MOVE_ERR( str ) {if ( bulk_write( c->s, str "\n", sizeof(str "\n") ) <= 0 ) c->do_work = 0; return;}

void game_proc_move( game_data_t *g, cli_data_t *c, cli_data_t *c2, int poss, int posd )
{
    if ( c->player_num != g->moving_player ) 
      MOVE_ERR( "Not your move" );
    if ( poss >= BOARD_SIZE ) 
      MOVE_ERR( "Source position too big" );
    if ( posd >= BOARD_SIZE ) 
      MOVE_ERR( "Destination position too big" );
    if ( g->board[ poss ] == ' ' ) 
      MOVE_ERR( "Source field is empty!" );
    if ( (g->board[ poss ] == '*') ^ c->player_num ) 
      MOVE_ERR( "Not your disc!" );
    if ( g->board[ poss ] == g->board[ posd ] ) 
      MOVE_ERR( "Invalid move destination" );

    char buf[256];
    sprintf(buf,"%c%u -> %c%u",poss%BOARD_N + 'A',poss/BOARD_N+1,posd%BOARD_N + 'A',posd/BOARD_N+1);
    game_append_line( c->game_ptr, "moves", buf );

    g->board[ posd ] = g->board[ poss ];
    g->board[ poss ] = ' ';

    g->moving_player ^= 1;

    c->cmd_from_game = client_game_send_status;
    c->opponent_ptr->cmd_from_game = client_game_send_status;
    sprintf( c->opponent_ptr->cmd_buf, "Your move!\n" );
    kill( c->opponent_ptr->pid, SIGALRM );
}

void client_init_game( cli_data_t *c )
{
    GET_SHARED_MEM( c->game_ptr, c->game_mem_id );
    GET_SHARED_MEM( c->opponent_ptr, c->game_ptr->p[ ! c->player_num ]);
    shared_mem_set_autodestroy( c->game_mem_id );
}

void client_deinit_game( cli_data_t *c, cli_data_t *opponent_ptr )
{
    game_store_data( c->game_ptr, c, opponent_ptr );
    c->game_ptr->do_work = 0;
    kill( c->opponent_ptr->pid, SIGALRM );
    if (TEMP_FAILURE_RETRY( shmdt( c->game_ptr ) ) ) 
    	ERR( "shmdt" );
    if (TEMP_FAILURE_RETRY( shmdt( c->opponent_ptr ) ) ) 
    	ERR( "shmdt" );
    c->game_ptr = NULL;
    c->opponent_ptr = NULL;
    c->state = client_connected;
}

void start_game( key_t game_key, int p_id )
{
    int game_mem_id;
    game_data_t *g = shared_mem_set_const_key( game_key, &game_mem_id, sizeof(*g) );

    if ( ! g->do_work ) {
        g->do_work = 1;
        g->p[0] = p_id;
        g->key = game_key;

    } else {
        g->moving_player = 0;
        g->move_num = 0;
        g->p[1] = p_id;

        int i;
        for ( i=0; i<BOARD_SIZE; i++ ) 
            g->board[i] = ' ';
        
        for ( i=0; i<BOARD_N; i++ ) {
            g->board[ (BOARD_N*0)+i ] = '+';
            g->board[ (BOARD_N*1)+i ] = '+';
            g->board[ (BOARD_N*(BOARD_N-2))+i ] = '*';
            g->board[ (BOARD_N*(BOARD_N-1))+i ] = '*';
        }

        cli_data_t *c;
        for ( i=0; i<2; i++ ) {
            GET_SHARED_MEM( c, g->p[i] );
            c->player_num = i;
            c->cmd_from_game = client_game_started;
            c->game_mem_id = game_mem_id;
            if ( !i ) 
              game_load_data( g, c );
            WAKE_CLIENT();
            if ( TEMP_FAILURE_RETRY(shmdt( c )) ) 
              ERR("shmdt");
        }
    }

    if ( TEMP_FAILURE_RETRY(shmdt( g )) ) 
        ERR("shmdt");
}

void client_process_game_command( cli_data_t *c )
{
    if ( c->cmd_buf[0] ) {
        if ( bulk_write( c->s, c->cmd_buf, strlen(c->cmd_buf) ) < 0 ) 
            c->do_work = 0;
        c->cmd_buf[0] = 0;
    }
    
    if ( c->state == client_playing && ! c->game_ptr->do_work ) {
         client_deinit_game( c, c->opponent_ptr );
         WRITE_CMD( "You can play another game or exit." );
    }

    int i, it;
    while ( c->cmd_from_game != -1 )
    {
          switch( c->cmd_from_game )
          {
            case client_game_started: // game started
                client_init_game( c );
                WRITE_CMD( "Game started" );
                c->state = client_playing;
                c->cmd_from_game = client_game_send_status;
                break;

            case client_game_send_status: // send status
                WRITE_CMD( "Board:" );
                it = 0;
                for ( i=0; i<BOARD_SIZE; i++ ) {
                    if (!(i%BOARD_N)) {
                        c->buf[it++] = '\n';
                        c->buf[it++] = ( '1' + i/BOARD_N );
                    }
                    c->buf[it++]=c->game_ptr->board[i];
                }
                c->buf[it++]='\n'; c->buf[it++]=' ';
                for ( i=0; i<BOARD_N; i++ ) c->buf[it++]='A' + i;
                c->buf[it++]='\n';
                c->buf[it++]='\n';
                if ( bulk_write( c->s, c->buf, it ) < 0 ) 
                  c->do_work = 0;
                c->cmd_from_game = -1;
                break;
          }
    }
}


void client_process_network_command( cli_data_t *c )
{
  if ( recv_line( c->s, c->buf,sizeof(c->buf) ) >= 0 )
  {
    switch ( c->buf[0] )
    {
        case 'E':
            c->do_work = 0;
            break;
        case 'P':
            if (c->id!=-1) { 
              ERR("Player is already authenticated"); 
              break; 
            }
            if (sscanf(c->buf+1,"%*[ ]%u",& c->id) != 1) 
              WRITE_CMD("Invalid command. Format: P player_id");
            break;
        case 'M':
            if ( c->state != client_playing ) { 
              WRITE_CMD( "Game inactive" ); 
              break; 
            }
            {
              unsigned int y,yd;
              char x,xd;
              if ( sscanf(c->buf+1,"%*[ ]%c%u%*[ ]%c%u",&x,&y,&xd,&yd) != 4 ) {
                WRITE_CMD( "Invalid command. Format: M x y xdest ydest" ); 
                break; 
              }

              game_proc_move( c->game_ptr, c, c->opponent_ptr, BOARD_N*(y-1)+(x-'A'), BOARD_N*(yd-1)+(xd-'A') );
            }
            break;

        case 'S':
            if ( c->state == client_connected )
            {
                if ( c->id == -1 ) {
                	WRITE_CMD( "Player not authenticated" ); 
                	break; 
                }
                if ( sscanf(c->buf+1,"%u",&c->game_key) != 1 ) { 
                	WRITE_CMD( "Invalid command. Format: S game_id" ); 
                	break; 
                }
                c->game_key = c->game_key * 3257422 + 283865;
                WRITE_CMD( "Waiting for other player..." );
                c->state = client_waiting;
                start_game( c->game_key, c->cli_mem_id );

            } else 
            	WRITE_CMD( "Client busy - can't start game." );
          break;

        case 'W':
            if ( c->state != client_playing ) { 
            	WRITE_CMD("game inactive"); 
            	break; 
            }
            client_send_msg( c->game_ptr, c, c->opponent_ptr, c->buf+1 );
            break;

        case 'L':
            game_show_file( c->game_ptr, c, "moves" );
            break;

        case 'A':
            client_show_file( c->game_ptr, c, "msg" );
            break;

        default:
            WRITE_CMD( "unknown command." );
            break;
    }

  } else  {
  	if (!(EAGAIN==errno||EINTR==errno))
    	c->do_work = 0;
  }
}


#define MENU_STRING "\n---- Game of checkers ----\n     by Andrzej Frankowski\n\nActions:\nE - exit\nP <id> - authenticate\nS <id> - start game\nM <x, y, xd, yd> - move\nW <msg> - write a message\nL - list all moves\nA - list all messages\n"



void do_client( int s )
{
    int mem_id;
    cli_data_t *c = shared_mem_set( &mem_id, sizeof( *c ) );
    shared_mem_set_autodestroy( mem_id );

    CLEAR( *c );
    c->cli_mem_id = mem_id;
    c->s = s;
    c->do_work = 1;
    c->pid = getpid();
    c->state = client_connected;
    c->cmd_from_game = -1;
    c->cmd_buf[0] = 0;
    c->id = -1;

    WRITE_CMD( MENU_STRING );

    c->game_ptr = NULL;
    c->opponent_ptr = NULL;

    while ( c->do_work ) {
        client_process_network_command( c );
        client_process_game_command( c );
    }

    switch( c->state )
    {
        case client_waiting:
            c->game_ptr->do_work = 0;
            if (TEMP_FAILURE_RETRY(shmdt( c->game_ptr ))) 
            	ERR( "shmdt" );
            break;
        case client_playing:
            client_deinit_game( c, c->opponent_ptr );
            break;
        case client_connected:
            break;
    }

    WRITE_CMD( "Goodbye" );
    if (TEMP_FAILURE_RETRY( close( c->s ) )) 
    	fprintf( stderr, "Closing socket failed.\n" );
    if (TEMP_FAILURE_RETRY( shmdt( c ) ) ) 
    	ERR( "shmdt" );
    exit( 0 );
}


void do_server( int fdU ) 
{
	volatile sig_atomic_t do_work = 1;

	while(do_work){
        int newfd = add_new_client( fdU );
        if ( newfd >= 0 ) {
            int p = fork();
            if ( p == -1 ) 
            	ERR("fork");
            if ( !p ) 
            	do_client( newfd );
            close( newfd );
        }
	}
}


int main(int argc, char** argv) 
{ 
	int fdU;
	if(argc!=2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

  	if ( mkdir( DATA_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH ) && errno != EEXIST ) 
      	ERR("mkdir");

	if(sethandler(sigint_handler,SIGINT)) 
    	ERR("Seting SIGINT:");
  	if(sethandler(sigalrm_handler,SIGALRM)) 
    	ERR("Seting SIGALRM:");

	fdU = bind_inet_socket(atoi(argv[1]), SOCK_STREAM);

	do_server(fdU);

	if(TEMP_FAILURE_RETRY(close(fdU))<0)
    ERR("close");

	fprintf(stderr,"Server has terminated.\n");

	return EXIT_SUCCESS;
}

