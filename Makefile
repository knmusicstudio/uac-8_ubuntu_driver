CC = gcc
CFLAGS = -Wall -O2 `pkg-config --cflags gtk+-3.0 jack libusb-1.0`
LIBS = `pkg-config --libs gtk+-3.0 jack libusb-1.0` -lpthread -lm
TARGET = uac8_bridge

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LIBS)

clean:
	rm -f $(TARGET)
