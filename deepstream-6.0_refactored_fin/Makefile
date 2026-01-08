################################################################################
# Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
################################################################################


APP:= deepstream-app

CUDA_VER=10.2

TARGET_DEVICE = $(shell g++ -dumpmachine | cut -f1 -d -)

NVDS_VERSION:=6.0

LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/
APP_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/bin/

ifeq ($(TARGET_DEVICE),aarch64)
  CFLAGS:= -DPLATFORM_TEGRA
endif

BASE_DIR := /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration

SRCS+= $(wildcard *.cpp)
SRCS+= $(wildcard *.c)

SRCS+= $(wildcard ../../apps-common/src/*.c)
SRCS+= $(wildcard */*.cpp)
SRCS+= $(wildcard */*/*.cpp)
SRCS+= $(wildcard */*/*/*.cpp)
SRCS+= $(wildcard -Wall (pkg-config --cflags --libs gstreamer-1.0 glib-2.0))
SRCS:= $(filter-out json/jsoncpp.cpp, $(SRCS))

INCS:= $(wildcard *.h)

PKGS:= gstreamer-1.0 gstreamer-video-1.0 opencv x11  json-glib-1.0

OBJS:= $(SRCS:.c=.o)

CFLAGS+= -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/aarch64-linux-gnu/glib-2.0/include \
		 -I../../apps-common/includes -I../../../includes -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 \
		 -I/usr/include/glib-2.0/glib -DDS_VERSION_MINOR=0 -DDS_VERSION_MAJOR=5 \
		 -I /usr/local/cuda-$(CUDA_VER)/include \
		 -I /usr/local/include/aws -I /usr/include/neon \
		 -I/usr/local/include -I/usr/src/jetson_multimedia_api/include \
		 -I $(BASE_DIR) \
		 -I $(BASE_DIR)/analytics \
		 -I $(BASE_DIR)/analytics/incident \
		 -I $(BASE_DIR)/analytics/queue \
		 -I $(BASE_DIR)/analytics/statistics \
		 -I $(BASE_DIR)/api \
		 -I $(BASE_DIR)/calibration \
		 -I $(BASE_DIR)/common \
		 -I $(BASE_DIR)/config \
		 -I $(BASE_DIR)/data/redis \
		 -I $(BASE_DIR)/data/sqlite \
		 -I $(BASE_DIR)/detection \
		 -I $(BASE_DIR)/detection/pedestrian \
		 -I $(BASE_DIR)/detection/special \
		 -I $(BASE_DIR)/detection/vehicle \
		 -I $(BASE_DIR)/image \
		 -I $(BASE_DIR)/json \
		 -I $(BASE_DIR)/monitoring \
		 -I $(BASE_DIR)/roi_module \
		 -I $(BASE_DIR)/server/core \
		 -I $(BASE_DIR)/server/manager \
		 -I $(BASE_DIR)/server/signal \
		 -I $(BASE_DIR)/server/source \
		 -I $(BASE_DIR)/server/source/manual \
		 -I $(BASE_DIR)/server/source/voltdb \
		 -I $(BASE_DIR)/spdlog \
		 -I $(BASE_DIR)/utils \
		 -I $(BASE_DIR)/utils/logger \
		 -I /usr/local/include/hiredis

LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lnvdsgst_smartrecord -lnvds_utils \
 	   -lnvbufsurface -lnvbufsurftransform -lnvdsgst_helper -lnvds_batch_jpegenc -lnvds_msgbroker -lm \
       -lgstrtspserver-1.0  -lgstrtp-1.0 -lcurl \
	   -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart -ldl -lneon\
	   -lstdc++ -lmysqlclient -lhiredis \
	   -ldl -Wl,-rpath,$(LIB_INSTALL_DIR) -Wl,-rpath,/usr/local/lib \
	   -L/usr/local/lib -lsqlite3 \
	   -lgstapp-1.0

CFLAGS+= `pkg-config --cflags $(PKGS)`
LIBS+= `pkg-config --libs $(PKGS)`

all: $(APP)

%.o: %.c $(INCS) Makefile
	$(CC) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CC) -g -Wall $(CFLAGS) -o ../../../../bin/$(APP) $(OBJS) $(LIBS) 

install: $(APP)
	cp -rv $(APP) $(APP_INSTALL_DIR)

clean:
	rm -rf *.o */*.o */*/*.o */*/*/*.o
	rm -rf ../../apps-common/src/*.o
	rm -rf ../../../../bin/$(APP)