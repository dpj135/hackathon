CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude

COMMON = src/json.cpp src/framework.cpp

.PHONY: all test score clean

all: nds_baseline nds_validate nds_generate nds_player nds_tests

nds_baseline: $(COMMON) src/baseline_main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

nds_validate: $(COMMON) src/validate_main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

nds_generate: $(COMMON) src/generate_main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

nds_player: $(COMMON) src/player_main.cpp player/solution.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

nds_tests: $(COMMON) tests/framework_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: all
	./nds_tests data
	bash scripts/smoke_test_generator.sh ./nds_generate ./nds_baseline ./nds_validate

score: nds_player nds_validate
	bash scripts/score.sh

clean:
	rm -f nds_baseline nds_validate nds_generate nds_player nds_tests
