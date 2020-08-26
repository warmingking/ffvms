src = $(wildcard *.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)

CFLAGS = -MMD
LDFLAGS = -Lhttp-parser -lhttp_parser -levent_core -lglog -lgflags -lstdc++

ffvms: $(obj) libhttp_parser
	$(CC) -o $@ $(obj) $(LDFLAGS)

-include $(dep)

.PHONY: clean cleandep libhttp_parser
# .FORCE: http_parser

clean:
	rm -rf ffvms $(obj)
	$(MAKE) -C http-parser clean

cleandep:
	rm -rf $(dep)

libhttp_parser:
	$(MAKE) -C http-parser package
