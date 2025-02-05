CC=/opt/cross/armv6/bin/arm-linux-musleabihf-gcc

all: convert

convert: convert.o
	$(CC) -o convert convert.o
convert.o: convert.c 
	$(CC) -c convert.c 

clean:
	rm -f *.o convert


