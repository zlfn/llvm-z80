//! elf2rel - Convert Z80 ELF object files to SDCC .rel (ASxxxx) format
//!
//! This tool bridges the LLVM/Clang/Rust Z80 toolchain (ELF output)
//! with the SDCC ecosystem (sdldz80/sdldgb linker, .rel/.lib libraries).
//!
//! .rel format (XL4):
//!   H <areas> areas <globals> global symbols
//!   M <module_name>
//!   A <area_name> size <hex> flags <hex> addr <hex>
//!   S <symbol_name> Def|Ref <hex_value>
//!   T <4-byte addr> <data bytes...>
//!   R <4-byte area_ref> [<4-byte reloc entries>...]
//!
//! R entry (4 bytes): mode, offset, index_lo, index_hi
//!   mode bit 0: R_BYTE (byte-sized reloc, vs word/16-bit)
//!   mode bit 1: R_SYM  (symbol reference, vs area reference)
//!   mode bit 2: R_PCR  (PC-relative)

use object::elf;
use object::read::elf::{ElfFile32, FileHeader, SectionHeader};
use object::read::elf::{Rela, Sym};
use object::LittleEndian as LE;
use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::Write;
use std::path::Path;
use std::process;

// Z80 ELF relocation types (from ELFRelocs/Z80.def)
const R_Z80_NONE: u32 = 0;
const R_Z80_IMM8: u32 = 1;
const R_Z80_ADDR8: u32 = 2;
const R_Z80_ADDR16: u32 = 3;
const R_Z80_ADDR16_LO: u32 = 4;
const R_Z80_ADDR16_HI: u32 = 5;
const R_Z80_PCREL_8: u32 = 6;
const R_Z80_ADDR24: u32 = 7;
const R_Z80_PCREL_16: u32 = 12;
const R_Z80_FK_DATA_4: u32 = 13;
const R_Z80_FK_DATA_8: u32 = 14;
const R_Z80_IMM16: u32 = 16;

// ASxxxx relocation mode bits
const R_BYTE: u8 = 0x01; // byte-sized (vs word/16-bit)
const R_SYM: u8 = 0x02; // symbol reference (vs area reference)
const R_PCR: u8 = 0x04; // PC-relative

/// Standard SDCC areas in the order they appear in .rel files
const SDCC_AREAS: &[(&str, u8)] = &[
    ("_CODE", 0),
    ("_DATA", 0),
    ("_INITIALIZED", 0),
    ("_DABS", 8), // absolute
    ("_HOME", 0),
    ("_GSINIT", 0),
    ("_GSFINAL", 0),
    ("_INITIALIZER", 0),
    ("_CABS", 8), // absolute
];

/// Maps ELF section names to SDCC area names
fn section_to_area(name: &str) -> &'static str {
    if name == ".text" || name.starts_with(".text.") {
        "_CODE"
    } else if name == ".data" || name.starts_with(".data.") {
        "_DATA"
    } else if name == ".bss" || name.starts_with(".bss.") {
        "_DATA"
    } else if name == ".rodata" || name.starts_with(".rodata.") {
        "_CODE"
    } else {
        "_CODE"
    }
}

/// A relocation to emit in the .rel file
struct RelReloc {
    /// Byte offset from T line start (including 4-byte address)
    offset: u8,
    /// Mode bits (R_BYTE, R_SYM, R_PCR)
    mode: u8,
    /// Index into symbol or area table
    index: u16,
}

/// Collected data for one area
struct AreaData {
    /// Raw code/data bytes
    bytes: Vec<u8>,
    /// Relocations
    relocs: Vec<(u32, RelReloc)>, // (byte_offset_in_area, reloc)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: elf2rel <input.o> [output.rel]");
        process::exit(1);
    }

    let input_path = &args[1];
    let output_path = if args.len() > 2 {
        args[2].clone()
    } else {
        let p = Path::new(input_path);
        p.with_extension("rel").to_string_lossy().into_owned()
    };

    let data = fs::read(input_path).unwrap_or_else(|e| {
        eprintln!("error: cannot read '{}': {}", input_path, e);
        process::exit(1);
    });

    let elf = ElfFile32::<LE>::parse(&*data).unwrap_or_else(|e| {
        eprintln!("error: cannot parse ELF '{}': {}", input_path, e);
        process::exit(1);
    });

    let header = elf.elf_header();
    let endian = LE;

    // Verify it's a Z80 ELF
    let machine = header.e_machine.get(endian);
    if machine != 0x1F90 {
        // EM_Z80 = 8080 = 0x1F90
        eprintln!(
            "error: not a Z80 ELF file (e_machine=0x{:04X}, expected 0x1F90)",
            machine
        );
        process::exit(1);
    }

    let sections = header.sections(endian, &*data).unwrap();
    let symbols_table = sections.symbols(endian, &*data, elf::SHT_SYMTAB).unwrap();

    // Build area table: index 0 = .__.ABS. (implicit), then SDCC standard areas
    // We track which areas have content
    let mut area_sizes: HashMap<&str, u32> = HashMap::new();
    let mut area_data: HashMap<&str, AreaData> = HashMap::new();

    for area in SDCC_AREAS {
        area_sizes.insert(area.0, 0);
        area_data.insert(
            area.0,
            AreaData {
                bytes: Vec::new(),
                relocs: Vec::new(),
            },
        );
    }

    // Map from ELF section index to (area_name, offset_within_area)
    let mut section_area_map: HashMap<usize, (&str, u32)> = HashMap::new();

    // First pass: collect section data into areas
    for (si, section) in sections.iter().enumerate() {
        let sh_type = section.sh_type(endian);
        if sh_type != elf::SHT_PROGBITS && sh_type != elf::SHT_NOBITS {
            continue;
        }

        let sh_flags = section.sh_flags(endian);
        if sh_flags & u32::from(elf::SHF_ALLOC) == 0 {
            continue;
        }

        let name = match sections.section_name(endian, section) {
            Ok(n) => {
                let n = std::str::from_utf8(n).unwrap_or("");
                if n.is_empty() {
                    continue;
                }
                n
            }
            Err(_) => continue,
        };

        // Skip non-code/data sections
        if name.starts_with(".note") || name.starts_with(".comment") {
            continue;
        }

        let area_name = section_to_area(name);
        let area = area_data.get_mut(area_name).unwrap();
        let offset = area.bytes.len() as u32;
        section_area_map.insert(si, (area_name, offset));

        if sh_type == elf::SHT_PROGBITS {
            let section_data = section.data(endian, &*data).unwrap_or(&[]);
            area.bytes.extend_from_slice(section_data);
        } else {
            // SHT_NOBITS (.bss)
            let size = section.sh_size(endian) as usize;
            area.bytes.resize(area.bytes.len() + size, 0);
        }

        *area_sizes.get_mut(area_name).unwrap() = area.bytes.len() as u32;
    }

    // Build symbol table for .rel output
    // Symbol 0 is always .__.ABS.
    struct RelSymbol {
        name: String,
        is_defined: bool,
        value: u32,
        /// Which area this symbol is defined in (area index in SDCC_AREAS)
        area_index: Option<usize>,
    }

    let mut rel_symbols: Vec<RelSymbol> = vec![RelSymbol {
        name: ".__.ABS.".to_string(),
        is_defined: true,
        value: 0,
        area_index: None,
    }];

    // Map from ELF symbol index to rel symbol index
    let mut elf_sym_to_rel: HashMap<u32, usize> = HashMap::new();

    // Collect global symbols
    for (sym_idx, sym) in symbols_table.iter().enumerate() {
        let bind = sym.st_bind();
        if bind != elf::STB_GLOBAL && bind != elf::STB_WEAK {
            continue;
        }

        let name = match symbols_table.symbol_name(endian, sym) {
            Ok(n) => {
                let n = std::str::from_utf8(n).unwrap_or("");
                if n.is_empty() {
                    continue;
                }
                n
            }
            Err(_) => continue,
        };

        let is_defined = sym.st_shndx(endian) != elf::SHN_UNDEF;
        let mut value = 0u32;
        let mut area_index = None;

        if is_defined {
            let sec_idx = sym.st_shndx(endian) as usize;
            if let Some(&(area_name, area_offset)) = section_area_map.get(&sec_idx) {
                // Find SDCC area index for this area
                for (ai, &(aname, _)) in SDCC_AREAS.iter().enumerate() {
                    if aname == area_name {
                        area_index = Some(ai);
                        break;
                    }
                }
                value = area_offset + sym.st_value(endian);
            }
        }

        let rel_idx = rel_symbols.len();
        rel_symbols.push(RelSymbol {
            name: name.to_string(),
            is_defined,
            value,
            area_index,
        });
        elf_sym_to_rel.insert(sym_idx as u32, rel_idx);
    }

    // Process relocations
    for (si, section) in sections.iter().enumerate() {
        let sh_type = section.sh_type(endian);
        if sh_type != elf::SHT_RELA {
            continue;
        }

        // Find the target section for this rela section
        let target_si = section.sh_info(endian) as usize;
        let (area_name, area_base_offset) = match section_area_map.get(&target_si) {
            Some(v) => *v,
            None => continue,
        };

        let area_idx = SDCC_AREAS
            .iter()
            .position(|&(n, _)| n == area_name)
            .unwrap();

        let rela_data = section.data(endian, &*data).unwrap_or(&[]);
        let relas =
            object::slice_from_bytes::<elf::Rela32<LE>>(rela_data, rela_data.len() / 8).unwrap().0;

        let area = area_data.get_mut(area_name).unwrap();

        for rela in relas {
            let r_offset = rela.r_offset(endian);
            let r_type = rela.r_type(endian);
            let r_sym = rela.r_sym(endian);
            let r_addend = rela.r_addend(endian);

            if r_type == R_Z80_NONE {
                continue;
            }

            // Determine relocation size and mode
            let (size, mut mode) = match r_type {
                R_Z80_ADDR16 | R_Z80_IMM16 => (2, 0u8),
                R_Z80_ADDR8 | R_Z80_IMM8 => (1, R_BYTE),
                R_Z80_PCREL_8 => (1, R_BYTE | R_PCR),
                R_Z80_PCREL_16 => (2, R_PCR),
                R_Z80_ADDR16_LO => (1, R_BYTE), // low byte of 16-bit
                R_Z80_ADDR16_HI => (1, R_BYTE), // high byte - needs special handling
                R_Z80_FK_DATA_4 => (4, 0), // DWARF, skip for now
                R_Z80_FK_DATA_8 => (8, 0), // DWARF, skip for now
                _ => {
                    eprintln!(
                        "warning: unsupported relocation type {} at offset 0x{:04X}",
                        r_type, r_offset
                    );
                    continue;
                }
            };

            // Skip DWARF relocations (not relevant for SDCC linking)
            if r_type == R_Z80_FK_DATA_4 || r_type == R_Z80_FK_DATA_8 {
                continue;
            }

            // Determine if this is a symbol or area reference
            let (reloc_index, is_sym_ref) = if r_sym != 0 {
                let sym = symbols_table.symbol(object::SymbolIndex(r_sym as usize)).unwrap();
                let bind = sym.st_bind();

                if bind == elf::STB_GLOBAL || bind == elf::STB_WEAK {
                    // Global/weak symbol → symbol reference
                    match elf_sym_to_rel.get(&r_sym) {
                        Some(&idx) => (idx as u16, true),
                        None => {
                            eprintln!("warning: unmapped symbol index {}", r_sym);
                            continue;
                        }
                    }
                } else {
                    // Local symbol → area reference
                    let sec_idx = sym.st_shndx(endian) as usize;
                    if let Some(&(_, _)) = section_area_map.get(&sec_idx) {
                        (area_idx as u16, false)
                    } else {
                        continue;
                    }
                }
            } else {
                (area_idx as u16, false)
            };

            if is_sym_ref {
                mode |= R_SYM;
            }

            // Apply addend to the code bytes
            let byte_offset = (area_base_offset + r_offset) as usize;

            // For symbol references, the addend goes into the code bytes
            // (SDCC linker adds symbol value to the bytes at link time)
            let addend = if !is_sym_ref {
                // Area reference: need to include the symbol's offset within the area
                let sym = symbols_table.symbol(object::SymbolIndex(r_sym as usize)).unwrap();
                let sym_sec = sym.st_shndx(endian) as usize;
                let sym_area_offset = section_area_map
                    .get(&sym_sec)
                    .map(|&(_, off)| off)
                    .unwrap_or(0);
                (r_addend as i64) + (sym.st_value(endian) as i64) + (sym_area_offset as i64)
            } else {
                r_addend as i64
            };

            // Write addend into code bytes
            if byte_offset < area.bytes.len() {
                match size {
                    1 => {
                        area.bytes[byte_offset] = (addend & 0xFF) as u8;
                    }
                    2 => {
                        if byte_offset + 1 < area.bytes.len() {
                            let val = addend as u16;
                            area.bytes[byte_offset] = (val & 0xFF) as u8;
                            area.bytes[byte_offset + 1] = ((val >> 8) & 0xFF) as u8;
                        }
                    }
                    _ => {}
                }
            }

            area.relocs.push((
                r_offset + area_base_offset,
                RelReloc {
                    offset: 0, // will be set when emitting T lines
                    mode,
                    index: reloc_index,
                },
            ));
        }
    }

    // Build the output symbol order and create a mapping from rel_symbols index
    // to output index. In .rel format, the symbol index in R lines corresponds
    // to the order S lines appear in the file.
    //
    // Order: .__.ABS. first, then Ref symbols, then Def symbols per area.
    let mut output_order: Vec<usize> = Vec::new(); // indices into rel_symbols
    output_order.push(0); // .__.ABS.

    for (i, sym) in rel_symbols.iter().enumerate().skip(1) {
        if !sym.is_defined {
            output_order.push(i);
        }
    }
    for (ai, _) in SDCC_AREAS.iter().enumerate() {
        for (i, sym) in rel_symbols.iter().enumerate().skip(1) {
            if sym.is_defined && sym.area_index == Some(ai) {
                output_order.push(i);
            }
        }
    }

    // Map from rel_symbols index to output S-line index
    let mut rel_to_output: HashMap<usize, u16> = HashMap::new();
    for (out_idx, &rel_idx) in output_order.iter().enumerate() {
        rel_to_output.insert(rel_idx, out_idx as u16);
    }

    // Remap relocation indices from rel_symbols index to output order
    for (_area_name, area) in area_data.iter_mut() {
        for (_, reloc) in area.relocs.iter_mut() {
            if reloc.mode & R_SYM != 0 {
                if let Some(&new_idx) = rel_to_output.get(&(reloc.index as usize)) {
                    reloc.index = new_idx;
                }
            }
        }
    }

    // Generate .rel output
    let mut out = Vec::new();
    let global_count = rel_symbols.len();

    writeln!(out, "XL4").unwrap();
    writeln!(out, "H {} areas {} global symbols", SDCC_AREAS.len(), global_count).unwrap();

    let module_name = Path::new(input_path)
        .file_stem()
        .unwrap_or_default()
        .to_string_lossy();
    writeln!(out, "M {}", module_name).unwrap();

    // Emit S lines in the output_order (before areas, then interleaved with areas)
    // First: .__.ABS. and Ref symbols
    writeln!(out, "S {} Def{:08X}", rel_symbols[0].name, rel_symbols[0].value).unwrap();
    for sym in &rel_symbols[1..] {
        if !sym.is_defined {
            writeln!(out, "S {} Ref{:08X}", sym.name, sym.value).unwrap();
        }
    }

    // Emit areas with their symbols and T/R lines together
    // (sdldz80 expects T/R immediately after each A line)
    for (ai, &(area_name, flags)) in SDCC_AREAS.iter().enumerate() {
        let size = area_sizes.get(area_name).copied().unwrap_or(0);
        writeln!(out, "A {} size {:X} flags {:X} addr 0", area_name, size, flags).unwrap();

        for sym in &rel_symbols[1..] {
            if sym.is_defined && sym.area_index == Some(ai) {
                writeln!(out, "S {} Def{:08X}", sym.name, sym.value).unwrap();
            }
        }

        let area = match area_data.get(area_name) {
            Some(a) if !a.bytes.is_empty() => a,
            _ => {
                // Empty area: no T/R lines (matches SDCC behavior)
                continue;
            }
        };

        // Split data into T lines (max ~16 data bytes per line for readability)
        const T_LINE_MAX: usize = 12;
        let mut offset = 0usize;

        while offset < area.bytes.len() {
            let chunk_end = (offset + T_LINE_MAX).min(area.bytes.len());
            let chunk = &area.bytes[offset..chunk_end];

            // T line: 4-byte address + data
            write!(out, "T {:02X} {:02X} 00 00", offset & 0xFF, (offset >> 8) & 0xFF).unwrap();
            for &b in chunk {
                write!(out, " {:02X}", b).unwrap();
            }
            writeln!(out).unwrap();

            // R line: area reference + relocations in this chunk
            write!(out, "R 00 00 {:02X} 00", ai).unwrap();

            // Find relocations within this chunk
            for &(reloc_offset, ref reloc) in &area.relocs {
                let reloc_off = reloc_offset as usize;
                if reloc_off >= offset && reloc_off < chunk_end {
                    // Offset in R entry is from T line start (including 4-byte address)
                    let t_offset = (reloc_off - offset + 4) as u8;
                    write!(
                        out,
                        " {:02X} {:02X} {:02X} {:02X}",
                        reloc.mode,
                        t_offset,
                        (reloc.index & 0xFF) as u8,
                        ((reloc.index >> 8) & 0xFF) as u8,
                    )
                    .unwrap();
                }
            }
            writeln!(out).unwrap();

            offset = chunk_end;
        }
    }

    // Write output
    fs::write(&output_path, &out).unwrap_or_else(|e| {
        eprintln!("error: cannot write '{}': {}", output_path, e);
        process::exit(1);
    });

    eprintln!("{} -> {}", input_path, output_path);
}
