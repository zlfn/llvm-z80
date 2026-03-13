//! rel2elf - Convert SDCC .rel/.lib files to Z80 ELF format
//!
//! Supports:
//!   .rel (SDCC object) → .o (ELF)
//!   .lib (ar archive of .rel) → .a (ar archive of ELF .o)

use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::Write;
use std::path::Path;
use std::process;

// Z80 ELF constants
const EM_Z80: u16 = 0x1F90;
const ET_REL: u16 = 1;
const ELFCLASS32: u8 = 1;
const ELFDATA2LSB: u8 = 1;
const EV_CURRENT: u8 = 1;
const SHT_NULL: u32 = 0;
const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_RELA: u32 = 4;
const SHT_NOBITS: u32 = 8;
const SHF_ALLOC: u32 = 0x2;
const SHF_WRITE: u32 = 0x1;
const SHF_EXECINSTR: u32 = 0x4;
const STB_LOCAL: u8 = 0;
const STB_GLOBAL: u8 = 1;
const STT_NOTYPE: u8 = 0;
const STT_SECTION: u8 = 3;
const SHN_UNDEF: u16 = 0;
const SHN_ABS: u16 = 0xFFF1;

// Z80 ELF relocation types
const R_Z80_ADDR8: u32 = 2;
const R_Z80_ADDR16: u32 = 3;
const R_Z80_PCREL_8: u32 = 6;
const R_Z80_PCREL_16: u32 = 12;

// ASxxxx relocation mode bits
const R_BYTE: u8 = 0x01;
const R_SYM: u8 = 0x02;
const R_PCR: u8 = 0x04;

const AR_MAGIC: &[u8; 8] = b"!<arch>\n";

// ── Ar archive helpers ──────────────────────────────────────────────────────

struct ArMember {
    name: String,
    data: Vec<u8>,
}

fn parse_ar(data: &[u8]) -> Result<Vec<ArMember>, String> {
    if data.len() < 8 || &data[..8] != AR_MAGIC {
        return Err("not an ar archive".into());
    }
    let mut pos = 8;
    let mut extended_names: Vec<u8> = Vec::new();
    let mut members = Vec::new();

    while pos + 60 <= data.len() {
        let header = &data[pos..pos + 60];
        if &header[58..60] != b"`\n" {
            return Err(format!("invalid ar header at offset {}", pos));
        }
        let name_raw = std::str::from_utf8(&header[0..16])
            .map_err(|_| "invalid ar member name")?
            .trim_end();
        let size_str = std::str::from_utf8(&header[48..58])
            .map_err(|_| "invalid ar member size")?
            .trim();
        let size: usize = size_str
            .parse()
            .map_err(|_| format!("invalid ar size: '{}'", size_str))?;
        pos += 60;

        if pos + size > data.len() {
            return Err("ar member exceeds file size".into());
        }
        let member_data = &data[pos..pos + size];
        pos += size;
        if pos % 2 != 0 {
            pos += 1;
        }

        if name_raw == "/" || name_raw == "/SYM64/" {
            continue;
        }
        if name_raw == "//" {
            extended_names = member_data.to_vec();
            continue;
        }

        let name = if let Some(stripped) = name_raw.strip_prefix('/') {
            let offset: usize = stripped
                .trim_end_matches('/')
                .trim()
                .parse()
                .map_err(|_| "invalid extended name offset")?;
            let end = extended_names[offset..]
                .iter()
                .position(|&b| b == b'/' || b == b'\n')
                .unwrap_or(extended_names.len() - offset);
            std::str::from_utf8(&extended_names[offset..offset + end])
                .map_err(|_| "invalid extended name")?
                .to_string()
        } else {
            name_raw.trim_end_matches('/').to_string()
        };

        members.push(ArMember {
            name,
            data: member_data.to_vec(),
        });
    }

    Ok(members)
}

fn write_ar(members: &[ArMember]) -> Vec<u8> {
    let mut buf = Vec::new();
    buf.extend_from_slice(AR_MAGIC);

    // Build extended name table for names that don't fit in 16 chars.
    // GNU ar format: names > 15 chars (including trailing '/') go into a '//' member,
    // and the header references them as "/<offset>".
    let mut ext_names = Vec::new();
    let mut name_offsets: Vec<Option<usize>> = Vec::new();
    for member in members {
        let short = format!("{}/", member.name);
        if short.len() <= 16 {
            name_offsets.push(None);
        } else {
            let offset = ext_names.len();
            ext_names.extend_from_slice(member.name.as_bytes());
            ext_names.extend_from_slice(b"/\n");
            name_offsets.push(Some(offset));
        }
    }

    // Write extended name table if needed.
    if !ext_names.is_empty() {
        write!(buf, "{:<16}", "//").unwrap();
        write!(buf, "{:<12}", "0").unwrap();
        write!(buf, "{:<6}", "0").unwrap();
        write!(buf, "{:<6}", "0").unwrap();
        write!(buf, "{:<8}", "0").unwrap();
        write!(buf, "{:<10}", ext_names.len()).unwrap();
        buf.extend_from_slice(b"`\n");
        buf.extend_from_slice(&ext_names);
        if ext_names.len() % 2 != 0 {
            buf.push(b'\n');
        }
    }

    for (i, member) in members.iter().enumerate() {
        let name_field = match name_offsets[i] {
            Some(offset) => format!("/{}", offset),
            None => format!("{}/", member.name),
        };
        write!(buf, "{:<16}", name_field).unwrap();
        write!(buf, "{:<12}", "0").unwrap();
        write!(buf, "{:<6}", "0").unwrap();
        write!(buf, "{:<6}", "0").unwrap();
        write!(buf, "{:<8}", "100644").unwrap();
        write!(buf, "{:<10}", member.data.len()).unwrap();
        buf.extend_from_slice(b"`\n");
        buf.extend_from_slice(&member.data);
        if member.data.len() % 2 != 0 {
            buf.push(b'\n');
        }
    }

    buf
}

// ── .rel parser ─────────────────────────────────────────────────────────────

struct RelArea {
    name: String,
    size: u32,
    #[allow(dead_code)]
    flags: u32,
    data: Vec<u8>,
    relocs: Vec<RelocEntry>,
}

struct RelocEntry {
    offset: u32,
    mode: u8,
    index: u16,
}

struct RelSymbol {
    name: String,
    is_defined: bool,
    value: u32,
    area_index: Option<usize>,
}

struct ParsedRel {
    #[allow(dead_code)]
    module: String,
    #[allow(dead_code)]
    addr_bytes: usize,
    areas: Vec<RelArea>,
    symbols: Vec<RelSymbol>,
}

fn parse_hex_bytes(s: &str) -> Vec<u8> {
    s.split_whitespace()
        .filter_map(|h| u8::from_str_radix(h, 16).ok())
        .collect()
}

fn parse_rel(text: &str) -> Result<ParsedRel, String> {
    let mut addr_bytes = 4usize;
    let mut module = String::new();
    let mut areas: Vec<RelArea> = Vec::new();
    let mut symbols: Vec<RelSymbol> = Vec::new();
    let mut current_area: Option<usize> = None;

    // Buffer last T line for pairing with R line
    let mut pending_t: Option<(u32, Vec<u8>)> = None;

    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        let first = match line.as_bytes().first() {
            Some(&b) => b,
            None => continue,
        };

        match first {
            b'X' => {
                // XL4 / XL2 etc.
                if line.len() >= 3 {
                    addr_bytes = (line.as_bytes()[2] - b'0') as usize;
                    if addr_bytes < 2 {
                        addr_bytes = 2;
                    }
                    if addr_bytes > 4 {
                        addr_bytes = 4;
                    }
                }
            }
            b'H' | b'O' => { /* skip header/options */ }
            b'M' => {
                if line.len() > 2 {
                    module = line[2..].trim().to_string();
                }
            }
            b'S' => {
                if line.len() < 4 {
                    continue;
                }
                let rest = &line[2..];
                // Find " Def" or " Ref" - search from the end since symbol names may contain spaces
                let (name, is_defined, value) =
                    if let Some(pos) = rest.rfind(" Def") {
                        let val_str = &rest[pos + 4..];
                        let val = u32::from_str_radix(val_str, 16).unwrap_or(0);
                        (&rest[..pos], true, val)
                    } else if let Some(pos) = rest.rfind(" Ref") {
                        let val_str = &rest[pos + 4..];
                        let val = u32::from_str_radix(val_str, 16).unwrap_or(0);
                        (&rest[..pos], false, val)
                    } else {
                        continue;
                    };

                symbols.push(RelSymbol {
                    name: name.to_string(),
                    is_defined,
                    value,
                    area_index: if is_defined { current_area } else { None },
                });
            }
            b'A' => {
                // A <name> size <hex> flags <hex> addr <hex>
                let parts: Vec<&str> = line[2..].split_whitespace().collect();
                if parts.len() < 6 {
                    continue;
                }
                let name = parts[0].to_string();
                let size = u32::from_str_radix(parts[2], 16).unwrap_or(0);
                let flags = u32::from_str_radix(parts[4], 16).unwrap_or(0);

                areas.push(RelArea {
                    name,
                    size,
                    flags,
                    data: vec![0u8; size as usize],
                    relocs: Vec::new(),
                });
                current_area = Some(areas.len() - 1);
            }
            b'T' => {
                let bytes = parse_hex_bytes(&line[2..]);
                if bytes.len() < addr_bytes {
                    continue;
                }
                let mut addr: u32 = 0;
                for i in 0..addr_bytes {
                    addr |= (bytes[i] as u32) << (i * 8);
                }
                let data = bytes[addr_bytes..].to_vec();
                pending_t = Some((addr, data));
            }
            b'R' => {
                let bytes = parse_hex_bytes(&line[2..]);
                if bytes.len() < 4 {
                    continue;
                }

                // First 4 bytes: area designator
                let area_idx = bytes[2] as usize | ((bytes[3] as usize) << 8);

                if let Some((t_addr, t_data)) = pending_t.take() {
                    // Commit T data to the area
                    if let Some(area) = areas.get_mut(area_idx) {
                        let start = t_addr as usize;
                        let needed = start + t_data.len();
                        if needed > area.data.len() {
                            area.data.resize(needed, 0);
                            area.size = needed as u32;
                        }
                        area.data[start..start + t_data.len()].copy_from_slice(&t_data);
                    }

                    // Parse relocation entries (groups of 4 bytes)
                    let mut i = 4;
                    while i + 4 <= bytes.len() {
                        let mode = bytes[i];
                        let t_offset = bytes[i + 1];
                        let index = bytes[i + 2] as u16 | ((bytes[i + 3] as u16) << 8);

                        let area_offset =
                            t_addr + (t_offset as u32) - (addr_bytes as u32);

                        if let Some(area) = areas.get_mut(area_idx) {
                            area.relocs.push(RelocEntry {
                                offset: area_offset,
                                mode,
                                index,
                            });
                        }

                        i += 4;
                    }
                }
            }
            _ => { /* ignore unknown line types */ }
        }
    }

    Ok(ParsedRel {
        module,
        addr_bytes,
        areas,
        symbols,
    })
}

// ── ELF writer ──────────────────────────────────────────────────────────────

fn push_u8(buf: &mut Vec<u8>, v: u8) {
    buf.push(v);
}
fn push_u16(buf: &mut Vec<u8>, v: u16) {
    buf.extend_from_slice(&v.to_le_bytes());
}
fn push_u32(buf: &mut Vec<u8>, v: u32) {
    buf.extend_from_slice(&v.to_le_bytes());
}
fn push_i32(buf: &mut Vec<u8>, v: i32) {
    buf.extend_from_slice(&v.to_le_bytes());
}

fn add_string(strtab: &mut Vec<u8>, s: &str) -> u32 {
    if s.is_empty() {
        return 0;
    }
    let offset = strtab.len() as u32;
    strtab.extend_from_slice(s.as_bytes());
    strtab.push(0);
    offset
}

fn area_to_section_name(area_name: &str) -> &str {
    match area_name {
        "_CODE" => ".text",
        "_DATA" => ".data",
        "_HOME" => ".text.home",
        "_GSINIT" => ".text.gsinit",
        "_GSFINAL" => ".text.gsfinal",
        "_INITIALIZED" => ".data.initialized",
        "_INITIALIZER" => ".rodata",
        "_DABS" => ".data.abs",
        "_CABS" => ".text.abs",
        other => other,
    }
}

fn area_section_flags(area_name: &str) -> u32 {
    match area_name {
        "_CODE" | "_HOME" | "_GSINIT" | "_GSFINAL" | "_CABS" => SHF_ALLOC | SHF_EXECINSTR,
        "_DATA" | "_INITIALIZED" | "_DABS" => SHF_ALLOC | SHF_WRITE,
        "_INITIALIZER" => SHF_ALLOC,
        _ => SHF_ALLOC,
    }
}

fn area_section_type(area_name: &str) -> u32 {
    match area_name {
        "_DATA" => {
            // _DATA could be BSS, but .rel doesn't distinguish;
            // if it has non-zero data, use PROGBITS
            SHT_PROGBITS
        }
        _ => SHT_PROGBITS,
    }
}

struct SectionEntry {
    name_offset: u32,
    sh_type: u32,
    sh_flags: u32,
    sh_offset: u32,
    sh_size: u32,
    sh_link: u32,
    sh_info: u32,
    sh_addralign: u32,
    sh_entsize: u32,
}

fn write_section_header(buf: &mut Vec<u8>, s: &SectionEntry) {
    push_u32(buf, s.name_offset); // sh_name
    push_u32(buf, s.sh_type);
    push_u32(buf, s.sh_flags);
    push_u32(buf, 0); // sh_addr
    push_u32(buf, s.sh_offset);
    push_u32(buf, s.sh_size);
    push_u32(buf, s.sh_link);
    push_u32(buf, s.sh_info);
    push_u32(buf, s.sh_addralign);
    push_u32(buf, s.sh_entsize);
}

fn convert_rel_to_elf(data: &[u8]) -> Result<Vec<u8>, String> {
    let text = std::str::from_utf8(data).map_err(|_| "invalid UTF-8 in .rel file")?;
    let parsed = parse_rel(text)?;

    // Collect non-empty areas that will become ELF sections
    struct AreaSection {
        area_idx: usize,
        section_idx: usize,
    }
    let mut area_sections: Vec<AreaSection> = Vec::new();
    let mut area_to_section_idx: HashMap<usize, usize> = HashMap::new();

    // Section index 0 = NULL, area sections start at 1
    let mut next_section_idx = 1usize;
    for (ai, area) in parsed.areas.iter().enumerate() {
        if area.size > 0 || !area.relocs.is_empty() {
            area_to_section_idx.insert(ai, next_section_idx);
            area_sections.push(AreaSection {
                area_idx: ai,
                section_idx: next_section_idx,
            });
            next_section_idx += 1;
        }
    }

    // Count areas with relocations → will need .rela sections
    let mut rela_section_indices: HashMap<usize, usize> = HashMap::new(); // area_idx → rela section idx
    for as_ in &area_sections {
        let area = &parsed.areas[as_.area_idx];
        if !area.relocs.is_empty() {
            rela_section_indices.insert(as_.area_idx, next_section_idx);
            next_section_idx += 1;
        }
    }

    let symtab_section_idx = next_section_idx;
    next_section_idx += 1;
    let strtab_section_idx = next_section_idx;
    next_section_idx += 1;
    let shstrtab_section_idx = next_section_idx;
    next_section_idx += 1;
    let total_sections = next_section_idx;

    // Build symbol table
    // Symbols: [NULL] [section symbols...] [global symbols...]
    let mut strtab = vec![0u8]; // starts with NUL
    let mut symtab_entries: Vec<Vec<u8>> = Vec::new();

    // Symbol 0: NULL
    {
        let mut entry = Vec::new();
        push_u32(&mut entry, 0); // st_name
        push_u32(&mut entry, 0); // st_value
        push_u32(&mut entry, 0); // st_size
        push_u8(&mut entry, 0); // st_info
        push_u8(&mut entry, 0); // st_other
        push_u16(&mut entry, SHN_UNDEF); // st_shndx
        symtab_entries.push(entry);
    }

    // Section symbols (one per area section)
    let mut area_to_sym_idx: HashMap<usize, u32> = HashMap::new(); // area_idx → symtab index
    for as_ in &area_sections {
        let sym_idx = symtab_entries.len() as u32;
        area_to_sym_idx.insert(as_.area_idx, sym_idx);

        let mut entry = Vec::new();
        push_u32(&mut entry, 0); // st_name (section symbols have no name)
        push_u32(&mut entry, 0); // st_value
        push_u32(&mut entry, 0); // st_size
        push_u8(&mut entry, (STB_LOCAL << 4) | STT_SECTION); // st_info
        push_u8(&mut entry, 0); // st_other
        push_u16(&mut entry, as_.section_idx as u16); // st_shndx
        symtab_entries.push(entry);
    }

    let first_global = symtab_entries.len() as u32;

    // Global symbols from S lines
    let mut sline_to_sym_idx: HashMap<usize, u32> = HashMap::new();
    for (si, sym) in parsed.symbols.iter().enumerate() {
        // Skip .__.ABS.
        if sym.name == ".__.ABS." {
            continue;
        }

        let sym_idx = symtab_entries.len() as u32;
        sline_to_sym_idx.insert(si, sym_idx);

        let name_offset = add_string(&mut strtab, &sym.name);

        let shndx = if sym.is_defined {
            if let Some(ai) = sym.area_index {
                area_to_section_idx.get(&ai).map(|&si| si as u16).unwrap_or(SHN_ABS)
            } else {
                SHN_ABS
            }
        } else {
            SHN_UNDEF
        };

        let value = if sym.is_defined { sym.value } else { 0 };

        let mut entry = Vec::new();
        push_u32(&mut entry, name_offset);
        push_u32(&mut entry, value);
        push_u32(&mut entry, 0); // st_size
        push_u8(&mut entry, (STB_GLOBAL << 4) | STT_NOTYPE);
        push_u8(&mut entry, 0);
        push_u16(&mut entry, shndx);
        symtab_entries.push(entry);
    }

    // Build rela sections
    // For each area with relocations, build .rela entries
    let mut rela_data_map: HashMap<usize, Vec<u8>> = HashMap::new(); // area_idx → rela bytes

    for as_ in &area_sections {
        let area = &parsed.areas[as_.area_idx];
        if area.relocs.is_empty() {
            continue;
        }

        let mut rela_bytes = Vec::new();
        for reloc in &area.relocs {
            let r_offset = reloc.offset;

            // Determine ELF relocation type
            let r_type = match (reloc.mode & R_BYTE != 0, reloc.mode & R_PCR != 0) {
                (true, true) => R_Z80_PCREL_8,
                (true, false) => R_Z80_ADDR8,
                (false, true) => R_Z80_PCREL_16,
                (false, false) => R_Z80_ADDR16,
            };

            // Determine symbol index and addend
            let (elf_sym, r_addend) = if reloc.mode & R_SYM != 0 {
                // Symbol reference - look up by S-line index
                let sym_idx = sline_to_sym_idx
                    .get(&(reloc.index as usize))
                    .copied()
                    .unwrap_or(0);

                // Read addend from data bytes
                let addend = read_addend(&area.data, r_offset as usize, reloc.mode & R_BYTE != 0);
                (sym_idx, addend)
            } else {
                // Area reference - use section symbol
                let ref_area_idx = reloc.index as usize;
                let sym_idx = area_to_sym_idx.get(&ref_area_idx).copied().unwrap_or(0);

                // Read addend from data bytes (includes offset within area)
                let addend = read_addend(&area.data, r_offset as usize, reloc.mode & R_BYTE != 0);
                (sym_idx, addend)
            };

            let r_info = (elf_sym << 8) | r_type;

            push_u32(&mut rela_bytes, r_offset);
            push_u32(&mut rela_bytes, r_info);
            push_i32(&mut rela_bytes, r_addend);
        }

        rela_data_map.insert(as_.area_idx, rela_bytes);
    }

    // Build shstrtab
    let mut shstrtab = vec![0u8];

    // Now lay out the ELF file
    let elf_header_size = 52u32;
    let section_header_size = 40u32;

    // Collect all section data and headers
    let mut section_headers: Vec<SectionEntry> = Vec::new();
    let mut section_data_blobs: Vec<Vec<u8>> = Vec::new();

    // Section 0: NULL
    section_headers.push(SectionEntry {
        name_offset: 0,
        sh_type: SHT_NULL,
        sh_flags: 0,
        sh_offset: 0,
        sh_size: 0,
        sh_link: 0,
        sh_info: 0,
        sh_addralign: 0,
        sh_entsize: 0,
    });
    section_data_blobs.push(Vec::new());

    // Area sections
    for as_ in &area_sections {
        let area = &parsed.areas[as_.area_idx];
        let sec_name = area_to_section_name(&area.name);
        let name_off = add_string(&mut shstrtab, sec_name);

        // Zero out relocation positions in data (RELA format stores addend separately)
        let mut sec_data = area.data.clone();
        for reloc in &area.relocs {
            let off = reloc.offset as usize;
            let is_byte = reloc.mode & R_BYTE != 0;
            if is_byte {
                if off < sec_data.len() {
                    sec_data[off] = 0;
                }
            } else {
                if off + 1 < sec_data.len() {
                    sec_data[off] = 0;
                    sec_data[off + 1] = 0;
                }
            }
        }

        // Check if all data is zero (potential BSS)
        let all_zero = sec_data.iter().all(|&b| b == 0) && area.relocs.is_empty();
        let sh_type = if all_zero && (area.name == "_DATA") {
            SHT_NOBITS
        } else {
            area_section_type(&area.name)
        };

        section_headers.push(SectionEntry {
            name_offset: name_off,
            sh_type,
            sh_flags: area_section_flags(&area.name),
            sh_offset: 0, // filled later
            sh_size: sec_data.len() as u32,
            sh_link: 0,
            sh_info: 0,
            sh_addralign: 1,
            sh_entsize: 0,
        });
        section_data_blobs.push(if sh_type == SHT_NOBITS {
            Vec::new()
        } else {
            sec_data
        });
    }

    // Rela sections
    for as_ in &area_sections {
        if let Some(rela_bytes) = rela_data_map.get(&as_.area_idx) {
            let rela_sec_name = format!(".rela{}", area_to_section_name(&parsed.areas[as_.area_idx].name));
            let name_off = add_string(&mut shstrtab, &rela_sec_name);

            section_headers.push(SectionEntry {
                name_offset: name_off,
                sh_type: SHT_RELA,
                sh_flags: 0,
                sh_offset: 0,
                sh_size: rela_bytes.len() as u32,
                sh_link: symtab_section_idx as u32,
                sh_info: as_.section_idx as u32,
                sh_addralign: 4,
                sh_entsize: 12, // sizeof(Elf32_Rela)
            });
            section_data_blobs.push(rela_bytes.clone());
        }
    }

    // .symtab
    let symtab_data: Vec<u8> = symtab_entries.iter().flatten().copied().collect();
    let symtab_name_off = add_string(&mut shstrtab, ".symtab");
    section_headers.push(SectionEntry {
        name_offset: symtab_name_off,
        sh_type: SHT_SYMTAB,
        sh_flags: 0,
        sh_offset: 0,
        sh_size: symtab_data.len() as u32,
        sh_link: strtab_section_idx as u32,
        sh_info: first_global,
        sh_addralign: 4,
        sh_entsize: 16, // sizeof(Elf32_Sym)
    });
    section_data_blobs.push(symtab_data);

    // .strtab
    let strtab_name_off = add_string(&mut shstrtab, ".strtab");
    section_headers.push(SectionEntry {
        name_offset: strtab_name_off,
        sh_type: SHT_STRTAB,
        sh_flags: 0,
        sh_offset: 0,
        sh_size: strtab.len() as u32,
        sh_link: 0,
        sh_info: 0,
        sh_addralign: 1,
        sh_entsize: 0,
    });
    section_data_blobs.push(strtab.clone());

    // .shstrtab
    let shstrtab_name_off = add_string(&mut shstrtab, ".shstrtab");
    section_headers.push(SectionEntry {
        name_offset: shstrtab_name_off,
        sh_type: SHT_STRTAB,
        sh_flags: 0,
        sh_offset: 0,
        sh_size: shstrtab.len() as u32,
        sh_link: 0,
        sh_info: 0,
        sh_addralign: 1,
        sh_entsize: 0,
    });
    section_data_blobs.push(shstrtab.clone());

    assert_eq!(section_headers.len(), total_sections);
    assert_eq!(section_data_blobs.len(), total_sections);

    // Calculate offsets
    let mut offset = elf_header_size;
    for (i, header) in section_headers.iter_mut().enumerate() {
        if header.sh_type == SHT_NULL {
            header.sh_offset = 0;
            continue;
        }
        if header.sh_type == SHT_NOBITS {
            header.sh_offset = offset;
            continue;
        }
        // Align
        let align = std::cmp::max(header.sh_addralign, 1);
        offset = (offset + align - 1) & !(align - 1);
        header.sh_offset = offset;
        offset += section_data_blobs[i].len() as u32;
    }

    // Align for section header table
    offset = (offset + 3) & !3;
    let sh_offset = offset;

    // Write ELF file
    let mut buf = Vec::new();

    // ELF header (52 bytes)
    buf.extend_from_slice(b"\x7fELF"); // e_ident magic
    push_u8(&mut buf, ELFCLASS32); // EI_CLASS
    push_u8(&mut buf, ELFDATA2LSB); // EI_DATA
    push_u8(&mut buf, EV_CURRENT); // EI_VERSION
    push_u8(&mut buf, 0); // EI_OSABI
    buf.extend_from_slice(&[0u8; 8]); // EI_ABIVERSION + padding
    push_u16(&mut buf, ET_REL); // e_type
    push_u16(&mut buf, EM_Z80); // e_machine
    push_u32(&mut buf, 1); // e_version
    push_u32(&mut buf, 0); // e_entry
    push_u32(&mut buf, 0); // e_phoff
    push_u32(&mut buf, sh_offset); // e_shoff
    push_u32(&mut buf, 0); // e_flags
    push_u16(&mut buf, elf_header_size as u16); // e_ehsize
    push_u16(&mut buf, 0); // e_phentsize
    push_u16(&mut buf, 0); // e_phnum
    push_u16(&mut buf, section_header_size as u16); // e_shentsize
    push_u16(&mut buf, total_sections as u16); // e_shnum
    push_u16(&mut buf, shstrtab_section_idx as u16); // e_shstrndx
    assert_eq!(buf.len(), 52);

    // Write section data
    for (i, header) in section_headers.iter().enumerate() {
        if header.sh_type == SHT_NULL || header.sh_type == SHT_NOBITS {
            continue;
        }
        // Pad to alignment
        while buf.len() < header.sh_offset as usize {
            buf.push(0);
        }
        buf.extend_from_slice(&section_data_blobs[i]);
    }

    // Pad to section header table offset
    while buf.len() < sh_offset as usize {
        buf.push(0);
    }

    // Write section header table
    for header in &section_headers {
        write_section_header(&mut buf, header);
    }

    Ok(buf)
}

fn read_addend(data: &[u8], offset: usize, is_byte: bool) -> i32 {
    if is_byte {
        if offset < data.len() {
            data[offset] as i8 as i32
        } else {
            0
        }
    } else {
        if offset + 1 < data.len() {
            let lo = data[offset] as u16;
            let hi = data[offset + 1] as u16;
            (lo | (hi << 8)) as i16 as i32
        } else {
            0
        }
    }
}

// ── .lib → .a conversion ───────────────────────────────────────────────────

fn convert_lib_to_ar(data: &[u8]) -> Result<Vec<u8>, String> {
    let members = parse_ar(data)?;
    let mut out_members = Vec::new();

    for member in &members {
        let o_name = Path::new(&member.name)
            .with_extension("o")
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned();

        match convert_rel_to_elf(&member.data) {
            Ok(elf_data) => {
                out_members.push(ArMember {
                    name: o_name,
                    data: elf_data,
                });
            }
            Err(e) => {
                eprintln!("warning: skipping '{}': {}", member.name, e);
            }
        }
    }

    Ok(write_ar(&out_members))
}

// ── Main ────────────────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: rel2elf <input.rel|input.lib> [output.o|output.a]");
        process::exit(1);
    }

    let input_path = &args[1];
    let data = fs::read(input_path).unwrap_or_else(|e| {
        eprintln!("error: cannot read '{}': {}", input_path, e);
        process::exit(1);
    });

    let is_ar = data.len() >= 8 && &data[..8] == AR_MAGIC;

    let default_ext = if is_ar { "a" } else { "o" };
    let output_path = if args.len() > 2 {
        args[2].clone()
    } else {
        Path::new(input_path)
            .with_extension(default_ext)
            .to_string_lossy()
            .into_owned()
    };

    let result = if is_ar {
        convert_lib_to_ar(&data)
    } else {
        convert_rel_to_elf(&data)
    };

    match result {
        Ok(output) => {
            fs::write(&output_path, &output).unwrap_or_else(|e| {
                eprintln!("error: cannot write '{}': {}", output_path, e);
                process::exit(1);
            });
            eprintln!("{} -> {}", input_path, output_path);
        }
        Err(e) => {
            eprintln!("error: {}", e);
            process::exit(1);
        }
    }
}
