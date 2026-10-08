CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g -I../../../comun/tests

TESTS = test_raiz_logica

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_raiz_logica: test_raiz_logica.cpp ../raiz_logica.h ../../../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
