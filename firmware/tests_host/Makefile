CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_protocolo test_buffer test_ingest_id

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_protocolo: test_protocolo.cpp ../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_buffer: test_buffer.cpp ../comun/buffer_circular.h ../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ingest_id: test_ingest_id.cpp ../comun/ingest_id.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
