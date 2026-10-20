top := ../../..
# initdb.c makes the storage it initializes.
INCLUDES := -I$(top)/../sm/include -I$(top)/../sq/include
include $(top)/dg.mk

# The flags the server's objects are built with, which "digres --build"
# prints.
main.$(OBJ): CPPFLAGS += -D'DG_BUILD_FLAGS="$(strip $(OPT) $(ASSERT))"'
