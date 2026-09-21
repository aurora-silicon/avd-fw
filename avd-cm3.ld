SECTIONS {
    . = 0x00000000;
    .vectors : { KEEP(*(.vectors)) }
    .text    : { *(.text*)   }
    .data    : { *(.data*)   }

    /* Bare-metal CM3 firmware has no unwinder.  LLVM otherwise places this
     * orphan section at address zero, ahead of the architectural vectors. */
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
