CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_enrolador test_estacion

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_enrolador: test_enrolador.cpp ../enrolador_logica.h ../../../comun/ntag424.h ../../../comun/sha256_hmac.h ../../../comun/tests/tarjeta_sim.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_estacion: test_estacion.cpp ../estacion_logica.h ../enrolador_logica.h ../../../comun/tests/aserciones.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
