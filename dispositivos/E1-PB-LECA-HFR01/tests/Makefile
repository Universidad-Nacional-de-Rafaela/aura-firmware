CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Ifakes
.PHONY: all clean
all: test_mesh test_storage test_envio
	./test_mesh
	./test_storage
	./test_envio
test_mesh: test_mesh.cpp mesh_assertions.h ../mesh_core.h
	$(CXX) $(CXXFLAGS) test_mesh.cpp -o $@
test_storage: test_storage.cpp fakes/Preferences.h ../mesh_storage.h ../mesh_core.h
	$(CXX) $(CXXFLAGS) test_storage.cpp -o $@
test_envio: test_envio.cpp fakes/Preferences.h ../envio_persistente.h ../modelo_envio.h ../politica_envio.h ../mesh_core.h ../../../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) test_envio.cpp -o $@

# Opcional: comprobar el parser real con la misma ArduinoJson del firmware.
ARDUINOJSON_INCLUDE ?=
json:
	$(CXX) $(CXXFLAGS) -I"$(ARDUINOJSON_INCLUDE)" test_config_json.cpp -o test_config_json
	./test_config_json

clean:
	rm -f test_mesh test_storage test_envio test_config_json *.exe
