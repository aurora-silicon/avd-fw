SECTIONS {
    . = 0x00000000;
    .vectors : { KEEP(*(.vectors)) }
    .rodata  : { *(.rodata*) }
    .text    : { *(.text*)   }
    .data    : { *(.data*)   }
    .bss     : { *(.bss*) *(COMMON) }
    ASSERT(ADDR(.bss) + SIZEOF(.bss) <= AVD_IMAGE_SIZE - 0x40, "firmware overlaps codec manifest")
    .codecs AVD_IMAGE_SIZE - 0x40 : { KEEP(*(.codecs)) }
    ASSERT(SIZEOF(.codecs) == 0x40, "codec manifest must occupy the final 64 bytes")

    /* Bare-metal CM3 firmware has no unwinder.  LLVM otherwise places this
     * orphan section at address zero, ahead of the architectural vectors. */
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
