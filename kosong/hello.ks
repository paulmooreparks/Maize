THE CODE:

if 1 = 1 
{
    print "Hello"
}
else
{
    print "Everything is not as it seems"
}

THE TREE:

root: {
    token: "if"
    literal: 1
    operator: =
    literal: 1
    block: {
        token: print
        string: "Hello"
    }
    token: else
    block: {
        token: print
        string: "Everything is not as it seems"
    }
}

token: [value: token] {
    expand(value)
}

operator: [operation: operator] {
    expand(operation)
}

decimal_literal: [prefix: operator [#], number: literal] {
    out(concat(prefix, literal))
}

hex_literal: [prefix: operator [$], hex_number: token [0-1,A-F,a-f] {
}

numeric_literal: expression [prefix: operator [#, $, %], value: literal] {
    template(prefix, value)
}

decimal_literal: numeric_literal [prefix: operator [#], value: literal] {
    concat(prefix,value)
}

expression: token [expr: token] {
    template(expr)
}

unary_expression: expression [expr: literal] {
    expr
}

unary_expression: expression [expr: string] {
    TEST expr
}

variable_unary_boolean: unary_boolean [expr: literal] {
    TEST expr
}

boolean_expression: 

if_statement: [if_token: "if", boolean_expr: boolean_eval, true_block: block, ? else_token: "else", ? false_block: block] {
    eval(boolean_expr)
    JZ if_true
    eval(false_block)
    JMP exit
    if_true:
    eval(true_block)
    exit:
}

CMP $01 $01
JZ if_true
else:
JMP exit
if_true:
LD A.H0 message
CALL print
exit:
HALT

message: "Hello\0"