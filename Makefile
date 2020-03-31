CC = gcc
CFLAGS = -g

EXECUTABLE := xs

all: $(EXECUTABLE)

OBJS := xs.o

xs : $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f $(EXECUTABLE) $(OBJS)
