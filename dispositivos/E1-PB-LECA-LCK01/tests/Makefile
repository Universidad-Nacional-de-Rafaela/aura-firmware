CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_locker test_cerradura test_config

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_locker: test_locker.cpp ../main/locker_logica.h ../main/cerradura_pulso.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_cerradura: test_cerradura.cpp ../main/cerradura_pulso.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_config: test_config.cpp ../main/config_validacion.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
