#
# Makefile — Kaypro display kernel (Circle bare metal, Pi Zero 2W)
# Place in ~/circle/app/kaypro/ alongside kernel.cpp
# Build: make RASPPI=2
#

CIRCLEHOME = ../..

OBJS = kernel.o

LIBS = $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
