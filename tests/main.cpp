#include "check.h"

#include "core/CastOnRules.h"

#include <cstdio>
#include <cstring>
#include <fstream>

int main(int argc, char** argv)
{
	// rdf_tests --write-schema <file>: the CastOn rule file JSON Schema (dist/CastOn/rdf_caston.schema.json)
	if (argc == 3 && std::strcmp(argv[1], "--write-schema") == 0) {
		const auto    schema = rdf::caston::JsonSchema();
		std::ofstream out(argv[2], std::ios::binary);
		out << schema;
		std::printf("wrote %s (%zu bytes)\n", argv[2], schema.size());
		return schema.empty() || !out ? 1 : 0;
	}
	return check::RunAll();
}
