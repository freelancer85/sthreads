DEBUG   := y
CC      := gcc
OS      := $(shell uname)
CFLAGS  := -std=gnu99 -Werror -Wall  -Wno-deprecated-declarations
LDFLAGS :=

ifeq ($(DEBUG), y)
	CFLAGS += -DDEBUG -g
endif

ifeq ($(OS), Linux)
	XCFLAGS := -pthread
	XLDLIBS := -lrt
endif

#NORMAL	 := timer
#UCONTEXT := contexts
NORMAL_TARGETS   := $(NORMAL)
UCONTEXT_TARGETS := $(UCONTEXT)
$(UCONTEXT_TARGETS): CFLAGS += -Wno-deprecated-declarations

.PHONY: all clean

all: $(NORMAL_TARGETS) $(UCONTEXT_TARGETS) sthreads_test

sthreads_test: sthreads_test.o sthreads.o sthreads.h
	$(CC) $(CFLAGS) $(LDLIBS) $(filter-out sthreads.h, $^) -o $@

sthreads.o: sthreads.c sthreads.h
	$(CC) $(CFLAGS) -c  $(filter-out sthreads.h, $^) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c  $< -o $@

clean:
	$(RM) *~ *.o  timer sthreads_test
	$(RM) -rf *.dSYM
