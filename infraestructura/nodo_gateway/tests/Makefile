CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g -I../../../comun/tests

TESTS = test_gateway_logica

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_gateway_logica: test_gateway_logica.cpp ../gateway_logica.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
