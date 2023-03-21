# The Kosong Programming Language

## Operators

    # base 10 literal (default)
    $ base 16 literal
    % base 2 literal
    : name to left of : is given to expression on right of :
    ^ address of value (obtain a reference)
    @ value at address (dereference a pointer)
    { begin scope
    } end scope
    ! executable code
    ~ optional parameter

## Keywords

    token       Any parsed token
    literal     A numeric value
    operator    One or more non-alphabetic characters not separated by whitespace
    string      A token that is a string of characters enclosed in quotes
    identifier  An alphanumeric value



expr: (value: ?) {}

literal_expr: (rvalue: literal, size: byte) (reg, subreg) {
    [literal]
}

reg: (reg_name: ["A", "B", "C", "D", "E", "G", "H", "J", "K", "L", "M", "Z"]) {
    [reg_name]
}

subreg: (subreg_name: ?) {}

reg_expr: (reg_name: reg, size: byte, subreg: ) (reg, subreg) {
    [reg].B[subreg]
}

unary_boolean: (op1: expr) {
    TEST op1
}

binary_boolean: (op1: expr, op2: expr) {
    CMP op1 op2
}

eval: (op1, op2) {
}



if: [op1: _word, op2: _word, _if_true: !, _else: ! ~ {
    CMP \op1\ \op2\
    JNZ _if_false;
    \_if_true\
    JMP _exit
    \_else\
    _exit:
}

fn: 

foo: int 42;
bar: int 42;

if [foo = bar] {
    print("equal");
}

_entry: [argc: word, argv: ] {
    greeting: auto fmt("Hello, {target}!", "world");
    print(greeting);
}

_exit: sub () {

}


foo: int < 0
bar: int < foo

5 > foo

