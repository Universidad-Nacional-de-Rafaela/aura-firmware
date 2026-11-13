CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_protocolo test_buffer test_ingest_id test_cola test_cola_archivos test_nodo_logica \
        test_aes_cmac test_ev2_auth test_ev2_seguro test_sha256_hmac test_ntag424 test_iso_dep test_iso14443a

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_protocolo: test_protocolo.cpp ../protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_buffer: test_buffer.cpp ../buffer_circular.h ../protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ingest_id: test_ingest_id.cpp ../ingest_id.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_cola: test_cola.cpp ../cola_persistente.h ../protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_cola_archivos: test_cola_archivos.cpp ../cola_archivos.h ../cola_persistente.h ../protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_nodo_logica: test_nodo_logica.cpp ../nodo_mesh_logica.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_aes_cmac: test_aes_cmac.cpp ../aes_cmac.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ev2_auth: test_ev2_auth.cpp ../ev2_auth.h ../aes_cmac.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ev2_seguro: test_ev2_seguro.cpp ../ev2_auth.h ../aes_cmac.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_sha256_hmac: test_sha256_hmac.cpp ../sha256_hmac.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ntag424: test_ntag424.cpp tarjeta_sim.h ../ntag424.h ../ev2_auth.h ../aes_cmac.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_iso_dep: test_iso_dep.cpp ../iso_dep.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_iso14443a: test_iso14443a.cpp ../iso14443a.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
