; tools/tree-sitter-binc/highlights.scm — BinC syntax highlighting queries
(keyword) @keyword
(identifier) @variable
(function_definition name: (identifier) @function)
(parameter name: (identifier) @parameter)
(field_declaration name: (identifier) @property)
(attribute name: (identifier) @attribute)
(constant_definition name: (identifier) @constant)
(struct_definition name: (identifier) @type)
(type) @type
(number) @number
(comment) @comment
"true" @boolean
"false" @boolean
"template" @keyword
"typename" @keyword
"struct" @keyword
"kernel" @keyword
"vertex" @keyword
"fragment" @keyword
"return" @keyword
"if" @keyword
"else" @keyword
"for" @keyword
"while" @keyword
"do" @keyword
"switch" @keyword
"case" @keyword
"default" @keyword
"break" @keyword
"continue" @keyword
"const" @keyword
"constant" @keyword
