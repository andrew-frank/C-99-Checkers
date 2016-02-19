proj_name= projekt1

all: $(proj_name)
$(proj_name): server.c	
	gcc -lrt -lpthread -Wall -O2 -o server server.c
.PHONY: clean
clean:
	rm server
run: $(proj_name)
	./server 10001
pack:
	tar zcvf b.tar.gz server.c Makefile


