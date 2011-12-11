#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include "bulletin.h"
#include "constants.h"
#include "client.h"


void submit_job_to_server(host_port host, job *to_send, data_size *files, int num_files) {
  int connection, i, err;
  char ip[INET_ADDRSTRLEN];
  i = bulletin_make_connection_with(host->host, host->port, &connection);
  if(i < 0) {
    problem("Connection to server failed\n");
    exit(-1);
  }
  i = ADD_JOB;
  do_rpc(&i);
  safe_send(connection, &num_files, sizeof(int));

  for(i = 0; i < num_files; i++) {
    err = safe_send(connection, &(files[i].size), sizeof(size_t));
    if(err) problem("Send failed\n");
    err = safe_send(connection, files[i]->data, size);
    if(err) problem("Send failed\n");
    err = safe_send(connection, files[i]->name, MAX_ARGUMENT_LEN*sizeof(char));
    if(err) problem("Send failed\n");
  }
  
  err = safe_send(connection, to_send, sizeof(job));
  if(err) problem("Send failed\n");
  safe_recv(connection, &i, sizeof(int));
  report_results(i);
}

int get_file_into_memory(FILE *file, data_size *location) {
  fseek(file , 0, SEEK_END);
  location->size = ftell(pFile);
  rewind(pFile);
  
  location->data = (char*) malloc(sizeof(char)*lSize);
  if (location->data == NULL) {fputs ("Memory error",stderr); exit (2);}
  
  // copy the file into the buffer:
  result = fread(location->data,1,location->size,file);
  if (result !=location->size ) {fputs ("Reading error",stderr); exit (3);}
  return OKAY;
}


void add_job_std_in(char *host, int port) {
  int num_args, num_jobs, connection, i, flags[MAX_ARGUMENTS];
  job *jobs;
  char job_names[MAX_JOBS][MAX_ARGUMENT_LEN], files[MAX_ARGUMENTS][BUFFER_SIZE];
  char temp[BUFFER_SIZE];
  jobs = malloc(sizeof(job)*num_jobs);


  printf("How many jobs would you like to enqueue?\n");
  fgets(temp, BUFFER_SIZE, stdin);
  sscanf(temp, "%d", &num_jobs);
  
  

  for(i = 0; i < num_jobs; i++) {
    printfl("Give the name of job %d", i);
    fgets(temp, MAX_ARGUMENT_LEN, stdin);
    lowercase(temp);
    strcpy(job_names[i], temp);
  }


  for(i = 0; i < num_jobs; i++) {
    do{
      printfl("Enter the names of the jobs that %s depends on separated by commas(only immediate dependence)",
	      job_names[i]);
      fgets(temp, BUFFER_SIZE-1, stdin);
    }  while(parse_dependencies(temp, job_names, &jobs[i]));
  }
  
}

void lowercase(char *str) {
  int i=0;
     while(str[i]) {
       str[i] = (char)tolower(str[i]);
       i++;
     }
}

int parse_dependencies(char *str, char job_names[MAX_JOBS][MAX_ARGUMENT_LEN], job *j) {
  char temp[BUFFER_SIZE];
  char *point;
  while(point = strchr(str, (int)',')) {
    strncpy(temp, str, str-point);
    printfl("%s", temp);
    str = point;
    str++;


  }
  
  return 0;
}



int main(int argc, char **argv) {
  add_job_std_in(NULL, 0);
}
