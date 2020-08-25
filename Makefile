LIB = -levent_core -lglog -lgflags

objects = ffvms.o rtsp_server.o

ffvms : $(objects)
	c++ -o ffvms $(objects) $(LIB)

ffvms.o : rtsp_server.h
rtsp_server.o : rtsp_server.h

.PHONY : clean
clean :
	rm -rf ffvms $(objects)
