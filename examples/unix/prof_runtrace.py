if not '__profiling__' in globals():
    raise Exception('Micropython Profiling feature is required!')

import sys

def format_instruction(instr):
    return "%s %s %s %s"%(
        instr[2],
        '{0!r}'.format(instr[3]) if instr[3] else "",
        '{0!r}'.format(instr[4]) if instr[4] else "",
        '{0!r}'.format(instr[5]) if instr[5] else "",
    )

def print_stacktrace(frame, level = 0):
    back = frame.f_back

    print("*%2d: %s0x%04x@%s:%s => %s:%d"%(
        level,
        "  " * level,

        frame.f_instroffset,
        frame.f_modulename,
        frame.f_code.co_codepath,
        

        frame.f_code.co_filename,
        frame.f_lineno,
    ))

    if (back):
        print_stacktrace(back, level + 1)

def print_bytecode(name, bytecode):
    for instr in bytecode:
        print("^0x%04x@%s\t%s"%(
            instr[0],
            name,
            format_instruction(instr)
        ))

class _Prof:
    display_flags = 0
    DISPLAY_INSTRUCITON = 1<<0
    DISPLAY_STACKTRACE = 1<<1

    instr_count = 0
    bytecodes = {}

    def instr_tick(self, frame, event, arg):
        if event == 'exception':
            print('!! exception at ',end='')
        else:
            self.instr_count += 1

        modulename = frame.f_modulename
        if not modulename in self.bytecodes:
            self.bytecodes[modulename] = sys.bytecode(modulename)

        # print(dir(frame.f_code))

        bytecode = self.bytecodes[modulename][frame.f_code.co_codepath]

        if self.display_flags & _Prof.DISPLAY_INSTRUCITON:
            for instr in bytecode:
                # print(instr)
                if instr[0] == frame.f_instroffset:
                    print(
                        ("{:<50}".format("# %2d: 0x%04x@%s:%s"%(
                            self.instr_count,
                            frame.f_instroffset,
                            frame.f_modulename,
                            frame.f_code.co_codepath,
                        )))+
                        format_instruction(instr)
                    )
                    # print(instr[2], instr[3], instr[4] if instr[4] else "")
                    break
        
        if self.display_flags & _Prof.DISPLAY_STACKTRACE:
            print_stacktrace(frame)


global __prof__
if not '__prof__' in globals():
    __prof__ = _Prof()
__prof__.display_flags |= _Prof.DISPLAY_INSTRUCITON

def instr_tick_handler(frame, event, arg):
    __prof__.instr_tick(frame, event, arg)

def atexit():
    print("\n------------------ script exited ------------------")
    print("Total instructions executed: ", __prof__.instr_count)
    print("Module __main__ bytecode of factorial dump:",)
    print_bytecode('factorial', __prof__.bytecodes['__main__']['/<module>/factorial'])

# Register atexit handler that will be executed before sys.exit().
sys.atexit(atexit)

# Start the tracing now.
sys.settrace(instr_tick_handler)


def factorial(n):
    if n == 0:
        # Display the bubbling stacktrace from this nested call.
        __prof__.display_flags |= _Prof.DISPLAY_STACKTRACE
        return 1
    else:
        return n * factorial(n-1)

def factorials_up_to(x):
    a = 1
    for i in range(1, x+1):
        a *= i
        yield a

# These commands are here to demonstrate some execution being traced.
print("Yo, this is wack!")

try:
    raise Exception('ExceptionallyLoud')
except:
    pass

# TODO FIXME This causes inconsistency in the codepath
# because both array generators have the same name.
# print("array_gen2 =>", [x**2 for x in range(10)] )
# print("array_gen3 =>", [x**3 for x in range(10)] )

print("factorials_up_to =>", list(factorials_up_to(3)))
print("factorial =>", factorial(4))

# Stop the tracing now.
sys.prof_mode(0)

# Trigger the atexit handler.
sys.exit()
