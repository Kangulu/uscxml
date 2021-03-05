#include <iostream>
#include "uscxml/uscxml.h"
#pragma comment(lib, "C:/Program Files (x86)/uSCXML/lib/uscxml.lib")

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cerr << "Expected URL with SCXML document as first argument" << std::endl;
		return -1;
	}

	uscxml::Interpreter sc = uscxml::Interpreter::fromURL(argv[1]);
	uscxml::InterpreterState state;
	while ((state = sc.step()) != uscxml::USCXML_FINISHED) {
	}

	return 0;
}
