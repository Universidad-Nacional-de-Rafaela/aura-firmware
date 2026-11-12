CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_regla

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

test_regla: test_regla.cpp ../main/regla_luz.h ../../../comun/tests/aserciones.h
	$(CXX) $(CXXFLAGS) -I../main -I../../../comun/tests -o $@ test_regla.cpp

clean:
	rm -f $(TESTS)
