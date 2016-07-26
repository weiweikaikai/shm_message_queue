RADE1_BIN=reader_1
RADE2_BIN=reader_2
WRITE_BIN=writer
READ_SRC1=shm_queue.c test_reader_1.c
READ_SRC2=shm_queue.c test_reader_2.c
WRITE_SRC=shm_queue.c test_writer.c
FLAGS=-g -Wall
INCLUDE=-I./
CC=gcc

.PHONY:all
all:$(RADE1_BIN) $(RADE2_BIN)  $(WRITE_BIN)
$(RADE1_BIN):$(READ_SRC1)	
	$(CC) $^ -o $@ $(FLAGS) $(INCLUDE)
$(RADE2_BIN):$(READ_SRC2)	
	$(CC) $^ -o $@ $(FLAGS) $(INCLUDE)
$(WRITE_BIN):$(WRITE_SRC)	
	$(CC) $^ -o $@ $(FLAGS) $(INCLUDE)
.PHONY:clean
clean:
	rm -rf  $(RADE1_BIN) $(RADE2_BIN) $(WRITE_BIN)