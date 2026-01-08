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

SRCS+= $(wildcard *.cpp)
SRCS+= $(wildcard *.c)

SRCS+= $(wildcard ../../apps-common/src/*.c)

SRCS+= $(wildcard */*.cpp)
SRCS+= $(wildcard */*/*.cpp)
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
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/calibration \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/config \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/logger \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/redis_client \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/roi_handle \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/server_comm \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/server_comm/backends \
		 -I /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/utils \
		 -I /usr/local/include/hiredis
# -I nvtlib/include/

LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lnvdsgst_smartrecord -lnvds_utils \
 	   -lnvbufsurface -lnvbufsurftransform -lnvdsgst_helper -lnvds_batch_jpegenc -lnvds_msgbroker -lm \
       -lgstrtspserver-1.0  -lgstrtp-1.0 -lcurl \
	   -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart -ldl -lneon\
	   -lstdc++ -lmysqlclient -lhiredis \
	   -ldl -Wl,-rpath,$(LIB_INSTALL_DIR) -Wl,-rpath,/usr/local/lib \
	   -L/usr/local/lib -lsqlite3 \
	   -lgstapp-1.0

# -L./nvtlib/lib/ -lnvtcam  -lnvtgentl

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













































# APP:= deepstream-app


# CUDA_VER=10.2

# TARGET_DEVICE = $(shell g++ -dumpmachine | cut -f1 -d -)

# NVDS_VERSION:=6.0

# LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/
# APP_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/bin/

# ifeq ($(TARGET_DEVICE),aarch64)
#   CFLAGS:= -DPLATFORM_TEGRA
# endif

# INCLUDES = -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/aarch64-linux-gnu/glib-2.0/include \
# 		 -I../../apps-common/includes -I../../../includes -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 \
# 		 -I/usr/include/glib-2.0/glib -DDS_VERSION_MINOR=0 -DDS_VERSION_MAJOR=5 \
# 		 -I /usr/local/cuda-$(CUDA_VER)/include \
# 		 -I /usr/local/include/aws -I /usr/include/neon \
# 		 -I /usr/local/include -I/usr/src/jetson_multimedia_api/include \
# 		 -I /usr/local/include/hiredis
# INCLUDES += $(addprefix -I , $(shell find /opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration -type d))

# C_SRCS := $(wildcard *.c) $(wildcard ../../apps-common/src/*.c)
# CPP_SRCS := $(wildcard *.cpp) $(wildcard */*.cpp) $(wildcard */*/*.cpp)

# C_OBJS := $(C_SRCS:.c=.o)
# CPP_OBJS := $(CPP_SRCS:.cpp=.o)
# OBJS := $(CPP_OBJS) $(C_OBJS)

# C_DEPS := $(C_SRCS:.c=.d)
# CPP_DEPS := $(CPP_SRCS:.cpp=.d)
# DEPS := $(C_DEPS) $(CPP_DEPS)

# INCS := $(wildcard *.h) $(wildcard */*.h) $(wildcard */*/*.h)
# PKGS:= gstreamer-1.0 gstreamer-video-1.0 opencv x11  json-glib-1.0

# CFLAGS += $(INCLUDES) -Wall -g -MMD -MP $(shell pkg-config --cflags $(PKGS))
# CXXFLAGS := $(CFLAGS)

# RMVS := $(OBJS) $(DEPS)
# LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lnvdsgst_smartrecord -lnvds_utils \
#  	   -lnvbufsurface -lnvbufsurftransform -lnvdsgst_helper -lnvds_batch_jpegenc -lnvds_msgbroker -lm \
#        -lgstrtspserver-1.0  -lgstrtp-1.0 -lcurl \
# 	   -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart -ldl -lneon\
# 	   -lstdc++ -lmysqlclient -lhiredis \
# 	   -ldl -Wl,-rpath,$(LIB_INSTALL_DIR) $(shell pkg-config --libs $(PKGS))\
# 	   -lsqlite3 \
# 	   -lgstapp-1.0

# all: $(APP)

# $(APP): $(OBJS)
# 	$(CC) $(CFLAGS) $(OBJS) -o $(BIN_DIR)/$(APP) $(LIBS)

# %.o: %.c $(INCS)
# 	$(CC) $(CFLAGS) -c -o $@ $<

# %.o: %.cpp $(INCS)
# 	$(CXX) $(CXXFLAGS) -c -o $@ $<

# install: $(APP)
# 	cp -rv $(APP) $(APP_INSTALL_DIR)

# clean:
# 	rm -rf $(RMVS) $(APP)



