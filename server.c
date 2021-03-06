#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include "network.h"
#include "constants.h"
#include "server.h"
#include "hash.h"
#include "runner.h"

//For add_lock
#define LOCKED 1
#define UNLOCKED 0

//////////GLOBALS//////////

//counter for Job IDs
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int counter = 0;

pthread_mutex_t server_list_mutex = PTHREAD_MUTEX_INITIALIZER;
host_list *server_list = NULL;

pthread_mutex_t failure_mutex = PTHREAD_MUTEX_INITIALIZER;
host_list *failed_hosts = NULL;

int *listener;
pthread_t listener_thread;
pthread_t user_thread;

host_list_node *my_host = NULL;
host_list_node *heartbeat_dest = NULL;

queue *my_queue;
queue *backup_queue;

pthread_mutex_t d_add_mutex = PTHREAD_MUTEX_INITIALIZER;
int d_add_lock = UNLOCKED;
char who[INET_ADDRSTRLEN];

//Not really viewed as separate from server.c, simply to divide
//the functions so they are more easily viewed
#include "rpc.c"
#include "failure.c"
#include "jobs.c"

int main(int argc, char **argv) {
  char name[INET_ADDRSTRLEN];
  pthread_t runner_thread;
  host_port *my_hostport;
  
  //Set our termination method
  signal(SIGINT, finish);
  signal(SIGPIPE, pipe_error);
  
  //Setup Jobs Folder
  if(mkdir("./jobs", S_IRWXU)) {
    if(errno == EEXIST) {
#ifdef VERBOSE2
      problem("Jobs directory already exists...\n");
#endif
    } else {
      problem("mkdir failed with %d\n", errno);
    }
  }
  
  //Setup Job Queue
  my_queue = (queue *)malloc(sizeof(queue));
  backup_queue = (queue *)malloc(sizeof(queue));
  init_queue(my_queue);
  init_queue(backup_queue);

  //Build host_port
  my_hostport = malloc(sizeof(host_port));
  get_my_ip(my_hostport->ip);
  my_hostport->port = atoi(argv[1]);
  while(my_hostport->port < 1000) 
    scanf("%d", &(my_hostport->port));
  my_hostport->jobs = 0;
  my_hostport->time_stamp = 0;
  my_hostport->location = 0;
  my_hostport->id = 0;
  
  //Start listener
  listener_set_up(my_hostport);

  if(argc < 4) {
    server_list = new_host_list(my_hostport);
    my_host = server_list->head;
  } else {
    if(get_servers(argv[2], atoi(argv[3]), 1, &server_list) < 0) {
      problem("Get servers failed!\n");
      exit(FAILURE);
    }
    if(acquire_add_lock(server_list)) {
      problem("Fatal error. Failed to acquire add lock.\n");
      relinquish_add_lock(server_list);
      finish(0);
      exit(-1);
    }
    my_hostport->id = server_list->id;
    server_list->id++;
    my_host = integrate_host(my_hostport);
    distribute_update();
    relinquish_add_lock(server_list);
  }
  heartbeat_dest = my_host->next;
  
  //User interaction thread
  pthread_create(&user_thread, NULL, (void *(*)(void *))print_method, NULL);
  
  //Runner/Heartbeat thread
  pthread_create(&runner_thread, NULL, (void *(*)(void *))runner, NULL);
  pthread_join(runner_thread, NULL);
}

//In Progress
int connect_to(host_list_node *server, int *connection) {
  make_connection_with(server->host->ip, server->host->port, connection);
}

//In Progress
int _do_rpc(int connection, char rpc) {
  return safe_send(connection, &rpc, sizeof(char));
}

int print_method() {
  char buffer[BUFFER_SIZE];
  while(1) {
    fgets(buffer, BUFFER_SIZE-1, stdin);
    if(!strcmp(buffer, EXIT))
      finish(0);
    printf(BAR);
    print_server_list();
    printf(BAR);
    printf("My Queue ");
    print_job_queue(my_queue);
    printf(BAR);
    printf("Backup Queue ");
    print_job_queue(backup_queue);
    printf(BAR);
  }
}

host_list_node *integrate_host(host_port *host) {
  host_list_node *runner, *max;
  int max_distance, dist;
  max_distance = 0;
  runner = server_list->head;
  do {
    dist = distance(runner->host->location, runner->next->host->location);
    if(!dist) dist = HASH_SPACE_SIZE;
    if(max_distance < dist) {
      max = runner;
      max_distance = dist;
    }
    runner = runner->next;
  } while(runner != server_list->head);
  host->location = max->host->location + (max_distance/2);
  return add_to_host_list(host, max);
}

void pipe_error(int sig) {
  problem("Pipe error\n");
}

void finish(int sig) {
  printf(BAR);
  printf("Finishing:\n");
  free(listener);
#ifdef VERBOSE
  printf("Freeing queues.\n");
#endif
  free_queue(my_queue);
  free_queue(backup_queue);
  if(server_list) {
#ifdef VERBOSE
    printf("Freeing server list.\n");
#endif
    free_host_list(server_list, 1);
  }
  if(failed_hosts) {
#ifdef VERBOSE
    printf("Freeing failed hosts list.\n");
#endif
    free_host_list(failed_hosts, 1);
  }
#ifdef VERBOSE
  printf("Destroying mutexes.\n");
#endif
  pthread_mutex_destroy(&count_mutex);
  pthread_mutex_destroy(&server_list_mutex);
  pthread_mutex_destroy(&failure_mutex);
  pthread_mutex_destroy(&d_add_mutex);
  if(listener_thread) {
#ifdef VERBOSE
    printf("Killing listener thread.\n");
#endif
    pthread_kill(listener_thread, SIGTERM);
  }
  printf(BAR);
  exit(0);
}

int heartbeat() {
  int connection, err;
  if(heartbeat_dest != my_host) {
    host_list *incoming;
    err = make_connection_with(heartbeat_dest->host->ip, heartbeat_dest->host->port, &connection);
    if (err < OKAY) {
      handle_failure(heartbeat_dest->host, 1);
      heartbeat_dest = heartbeat_dest->next;
      return FAILURE;
    }
    err = HEARTBEAT;
    do_rpc(&err);
    receive_host_list(connection, &incoming);
    pthread_mutex_lock(&(my_host->lock));
    my_host->host->time_stamp++;
    my_host->host->jobs = my_queue->active_jobs;
    pthread_mutex_unlock(&(my_host->lock));
    safe_send(connection, my_host->host, sizeof(host_port));
    update_job_counts(incoming);
    free_host_list(incoming, 1);
    close(connection);
  }
  heartbeat_dest = heartbeat_dest->next;
}

int update_job_counts(host_list *update) {
  host_list_node *my_runner, *update_runner;
  my_runner = server_list->head;
  update_runner = update->head;
  do {
    if(my_runner->host->id != update_runner->host->id) {
      problem("Heartbeat update did not agree with our current serverlist.\n");
    }
    if(my_runner->host->time_stamp < update_runner->host->time_stamp) {
      my_runner->host->time_stamp = update_runner->host->time_stamp;
      my_runner->host->jobs = update_runner->host->jobs;
    }
    my_runner = my_runner->next;
    update_runner = update_runner->next;
  } while(my_runner != server_list->head);  
}

int acquire_add_lock(host_list *list) {
  int err, connection;
  host_list_node *runner;
  runner = list->head;
  do {
    if(runner != my_host) {
      err = make_connection_with(runner->host->ip, runner->host->port, &connection);
      if (err < OKAY){
	return FAILURE;
      }
      err = request_add_lock(connection);
      if (err < OKAY){
	relinquish_add_lock(list);
	return FAILURE;
      }
      close(connection);
      runner = runner->next;
    }
  }  while(runner != list->head);
  return OKAY;
}

int relinquish_add_lock(host_list *list) {
  int err, connection;
  host_list_node *runner;
  runner = list->head;
  do {
    if(runner != my_host) {
      err = make_connection_with(runner->host->ip, runner->host->port, &connection);
      if (err < OKAY) {
	problem("Urgent, failure in reliquishing add_lock\n");
	handle_failure(runner->host, 1);
      } else {
	err = tell_to_unlock(connection);
      }
      close(connection);
    }
    runner = runner->next;
  }  while(runner != list->head);
  return OKAY;
}

int tell_to_unlock(int connection) {
  int num = UNLOCK;
  do_rpc(&num);
  num = FAILURE;
  safe_recv(connection, &num, sizeof(int));
  return num;
}

int request_add_lock(int connection) {
  int num = REQUEST_ADD_LOCK;
  do_rpc(&num);
  num = FAILURE;
  safe_recv(connection, &num, sizeof(int));
  return num;
}

int announce(int connection, host_port *send) {
  int status = ANNOUNCE;
  status = do_rpc(&status);
#ifdef VERBOSE2
  printf("Sending announce\n");
#endif
  if(status < 0){
    problem("Failed to acknowledge announce\n");
    problem("Send Failed\n");
    return status;
  }
  status = safe_send(connection, send, sizeof(host_port));
  if(status < 0) problem("Send Failed\n");
  return status;
}

void distribute_update() {
  int err;
  int connection;
  host_list_node* current_node;
  current_node = server_list->head;

  do {
    if(current_node != my_host) {
      err = make_connection_with(current_node->host->ip, 
				 current_node->host->port, &connection);
      if (err < OKAY) handle_failure(current_node->host, 1);
      err = announce(connection, my_host->host);
      close(connection);
    }
    current_node = current_node->next;
  } while(current_node != server_list->head);
}

int send_host_list(int connection, host_list *list) {
  int err, num;
  host_list_node *runner;
  host_port *hosts;
  runner = list->head;
  num = 0;

  //count number of hosts
  do {
    num++;
    runner = runner->next;
  } while(runner != list->head);
  hosts = malloc(sizeof(host_port)*num);
  runner = list->head;
  
  //pack the hosts into an array
  for(err = 0; err < num; err++) {
    memcpy(&hosts[err], runner->host, sizeof(host_port));
    runner = runner->next;
  }
  
  //send
  err = safe_send(connection, &num, sizeof(int));
  if(err < OKAY) return err;
  err = safe_send(connection, hosts, sizeof(host_port)*num);
  if(err < OKAY) return err;
  err = safe_send(connection, &(list->id), sizeof(unsigned int));
  if(err < OKAY) return err;

  return OKAY;
}

int receive_host_list(int connection, host_list **list) {
  int err, num, i;
  host_port *hosts, *temp;
  host_list_node *runner;
  unsigned int id;
  num = 0;
  //size of array
  err = safe_recv(connection, &num, sizeof(int));
  if(err < OKAY) return err;
  hosts = malloc(sizeof(host_port)*num);
  err = safe_recv(connection, hosts, sizeof(host_port)*num);
  if(err < OKAY) return err;
  err = safe_recv(connection, &id, sizeof(unsigned int));
  if(err < OKAY) return err;
  
  
  //we malloc new host ports so that freeing is easy later
  //we cant just use the memory we allocated earlier as an array
  //or else it is impossible to free individual elements of the list
  temp = malloc(sizeof(host_port));
  memcpy(temp, &hosts[0], sizeof(host_port));
  *list = new_host_list(temp);
  runner = (*list)->head;
  (*list)->id = id;
  for(i = 1; i < num; i++) {
    temp = malloc(sizeof(host_port));
    memcpy(temp, &hosts[i], sizeof(host_port));
    runner = add_to_host_list(temp, runner);
  }
  return OKAY;
}

void free_host_list(host_list *list, int flag) {
  host_list_node *runner = list->head;
  host_list_node *prev;
  do {
    if(runner->host && flag) free(runner->host);
    prev = runner;
    runner = runner->next;
    pthread_mutex_destroy(&(prev->lock));
    free(prev);
  } while(runner != list->head) ;
  free(list);
}

host_list *new_host_list(host_port *initial_host_port) {
  host_list *new_list;
  new_list = malloc(sizeof(host_list));
  new_list->head = malloc(sizeof(host_list_node));
  new_list->head->host = initial_host_port;
  new_list->head->next = new_list->head;
  new_list->head->prev = new_list->head;
  new_list->id = 1;
  pthread_mutex_init(&(new_list->head->lock), NULL);
  return new_list;
}

host_list_node *add_to_host_list(host_port *added_host_port, host_list_node *where_to_add) {
  host_list_node *new_node;
  new_node = (host_list_node *)malloc(sizeof(host_list_node));
  new_node->host = added_host_port;
  pthread_mutex_init(&(new_node->lock), NULL);

  new_node->prev = where_to_add;
  new_node->next = where_to_add->next;
  where_to_add->next = new_node;
  new_node->next->prev = new_node;
  return new_node;
}

void print_server_list() {
  host_list_node *current_node, *last = NULL;
  int count = 0;
  printf("Server List:\n");
  current_node = server_list->head;
  printf("%-5s   %-15s   %-5s   %-5s   %-5s   %-5s\n",
	 "id", "ip", "port", "hash", "#jobs", "stamp");
  do {
    if(last == current_node || count > 50) {
      problem("Print server list should have finished, but it did not\n");
      break;
    }
    last = current_node;
    printf("%-5u | %-15s | %-5u | %-5u | %-5u | %-5u", 
	   current_node->host->id, current_node->host->ip, current_node->host->port,
	   current_node->host->location, current_node->host->jobs, current_node->host->time_stamp);
#ifdef VERBOSE
    printf(" prev: %u, self: %u, next: %u", 
	   current_node->prev->host->id, current_node->host->id, current_node->next->host->id);
#endif
    if(current_node == my_host)
      printf(" (Me)");
    printf("\n");
    current_node = current_node->next;
  } while(current_node != server_list->head);
}

int get_servers(char *hostname, int port, int add_slots, host_list **list) {
  int connection = 0;
  int result = 0;
  int err;
  err = make_connection_with(hostname, port, &connection);
  if(err < 0) {
    problem("Failed to connect to retrive servers");
    return FAILURE;
  }
  err = SEND_SERVERS;
  do_rpc(&err);
  result = receive_host_list(connection, list);
  close(connection);
  return result;
}

void listener_set_up(host_port *info) {
  int connect_result;
  listener = malloc(sizeof(int));
  connect_result = set_up_listener(info->port, listener);
  pthread_create(&listener_thread, NULL, (void *(*)(void *))listen_for_connection, listener);
}

void listen_for_connection(int *listener) {
  int connection, connect_result;
  connect_result = -1;
  do {
    connect_result = wait_for_connection(*listener, &connection);
  } while(connect_result < 0);
  pthread_create(&listener_thread, NULL, (void *(*)(void *))
		listen_for_connection, listener);
  handle_rpc(connection);
}

void add_host_to_list_by_location(host_port *host, host_list *list) {
  host_list_node *runner = list->head;
  while(runner->next->host->location < host->location && runner->next->host->location != 0) {
    runner = runner->next;
  }
  add_to_host_list(host, runner);
}

int get_job_id(job *ajob) {
  pthread_mutex_lock(&count_mutex);
  ajob->id = my_host->host->id*JOB_ID_SPACER + counter;
  counter++;
  pthread_mutex_unlock(&count_mutex);
  return ajob->id;
}

int receive_file(int connection, data_size *file) {
  int err;
  err = safe_recv(connection, &(file->size), sizeof(size_t));
  if(err < 0) problem("Recv failed size\n");
  file->data = malloc(file->size);
  err = safe_recv(connection, file->data, file->size);
  if(err < 0) problem("Recv failed data\n");
  err = safe_recv(connection, file->name, MAX_ARGUMENT_LEN*sizeof(char));
  if(err < 0) problem("Recv failed name\n");
}
