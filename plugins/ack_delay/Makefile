SRC=$(wildcard *.c)
OBJ=$(SRC:.c=.o)
CFLAGS=-I../../picoquic -DDISABLE_PROTOOP_PRINTF -DACK_DELAY_MULT=$(ACK_DELAY_MULT) -DACK_DELAY_DIV=$(ACK_DELAY_DIV) -DACK_THRESHOLD=$(ACK_THRESHOLD)

all: $(SRC) $(OBJ)

$(OBJ): %.o

%.o: %.c
	clang-6.0 $(CFLAGS) -O2 -fno-gnu-inline-asm -emit-llvm -c $< -o - | llc-6.0 -march=bpf -filetype=obj -o $@

clean:
	rm -rf *.o

.PHONY: %.o