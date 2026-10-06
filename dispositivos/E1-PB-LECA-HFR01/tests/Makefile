CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Ifakes
.PHONY: all clean
all: test_mesh test_storage
	./test_mesh
	./test_storage
test_mesh: test_mesh.cpp mesh_assertions.h ../mesh_core.h
	$(CXX) $(CXXFLAGS) test_mesh.cpp -o $@
test_storage: test_storage.cpp fakes/Preferences.h ../mesh_storage.h ../mesh_core.h
	$(CXX) $(CXXFLAGS) test_storage.cpp -o $@
clean:
	rm -f test_mesh test_storage *.exe
