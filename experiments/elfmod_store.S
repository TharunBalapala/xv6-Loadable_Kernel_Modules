.attribute stack_align, 16
.section .text
.globl module_init
.type module_init, @function
module_init:
.Lpcrel_hi:
  auipc t0, %pcrel_hi(sample_data)
  sw a0, %pcrel_lo(.Lpcrel_hi)(t0)
  ret
.size module_init, .-module_init

.section .data, "aw", @progbits
.align 2
.globl sample_data
.type sample_data, @object
sample_data:
  .word 0
.size sample_data, .-sample_data
