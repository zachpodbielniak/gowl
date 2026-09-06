# Every module using core headers must rebuild with the matching libgowl ABI.
$(OUT): $(SRC) $(wildcard ../../src/core/*.h ../../src/interfaces/*.h ../../src/module/*.h) $(LIBDIR)/libgowl.so
