/*
 * parse_tool.h
 *	  "digres --parse": the parser run by hand.
 *
 * src/include/parser/parse_tool.h
 */
#ifndef PARSE_TOOL_H
#define PARSE_TOOL_H

/*
 * Parse standard input and print the result: statements, or one type
 * name if argv[2] is "--type-name". Returns the process's exit status.
 */
extern int parse_tool_main(int argc, char **argv);

#endif /* PARSE_TOOL_H */
