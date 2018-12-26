TARGET:= alsa_test
SOURCES := main.c audio_alsa.c
CFLAGS := -pthread
LIBS := -lpthread -lasound

objects := $(addprefix ,$(SOURCES:.c=.o))

.PHONY: all clean

all: $(objects)
	$(CC) -o $(TARGET) $(objects) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)
