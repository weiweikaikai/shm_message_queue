BIN=shmqueue
SRC=opt_time.h shm_queue.c
FLAGS=-g -Wall -DTEST_TIME
CC=gcc

$(BIN):$(SRC)	
	$(CC) $^ -o $@ $(FLAGS)
.PHONY:clean
clean:
	rm -rf  $(BIN)