doc: >
  Maintains a database of all books on the device for the purposes of
  sorting and searching. Written by LibraryBuilder to /.crosspoint/library.idx

meta:
  id: clix_format
  endian: le

seq:
  - id: header
    type: clix_header

instances:
  folders:
    pos: _root.header.folder_start
    size: _root.header.folder_len
    type: folder_section
    doc: List of folders in which books may live.
    
  records:
    pos: _root.header.record_start
    size: _root.header.book_count * 128
    type: record_section
    doc: List of entries in the library. Each entry is exactly 128 bytes.
    
  lists:
    pos: _root.header.list_start
    size: _root.header.list_len
    type: list_section
    doc: >
      Table of list descriptors, then their entries and labels. Lists 0..2 are
      Recent, Title and Author; later lists come from external tools.

  name_section:
    pos: _root.header.name_start
    size: _root.header.name_len
    type: name_section_raw
    doc: Raw byte container wrapping the collective pool of name strings.

types:
  clix_header:
    seq:
      - id: magic
        contents: "CLX1" 
      - id: format_version
        contents: [3]
      - id: fold_version
        contents: [5]
      - id: flags
        type: clix_flags
      - id: metadata_enabled
        type: b1
      - id: book_count
        type: u2
      - id: folder_count
        type: u2
        doc: Number of distinct folders in which books live
      - id: next_first_seen
        type: u2
      - id: padding_1
        type: u2
      - id: folder_start
        type: u4
        doc: Offset into file at which the Folder section starts.
      - id: folder_len
        type: u4
        doc: Total size of the meaningful data in the folder section.
      - id: record_start
        type: u4
        doc: Offset into file at which the ClixRecords start.
      - id: list_start
        type: u4
        doc: Offset into file at which the list section starts.
      - id: name_start
        type: u4
      - id: name_len
        type: u4
      - id: self_size
        type: u4
      - id: list_len
        type: u4
        doc: Total size of the list section.
      - id: list_count
        type: u2
        doc: Descriptors in the list table; at least 3.
      - id: reserved
        size: 14

    types:
      clix_flags:
        meta:
          bit-endian: le
        seq:
          - id: ranks_degraded
            type: b1
          - id: dedup_degraded
            type: b1
          - id: lists_dropped
            type: b1
            doc: External lists from the previous index could not be carried over.
          - id: reserved
            type: b5

  folder_section:
    seq:
      - id: entries
        type: u1_prefixed_string
        repeat: expr
        repeat-expr: _root.header.folder_count

  u1_prefixed_string:
    seq:
      - id: len
        type: u1
      - id: value
        type: str
        size: len
        encoding: UTF-8

  record_section:
    seq:
      - id: entries
        type: clix_record
        repeat: expr
        repeat-expr: _root.header.book_count

  clix_record:
    seq:
      - id: name_offset
        doc: Offset from nameStart into the per-record name blob
        type: u4
      - id: file_size
        doc: Captured while the dirent was open, part of the identity
        type: u4 
      - id: first_seen
        type: u2
      - id: folder_id
        type: u2
        doc: Index into the array of entries in the folder_section.
      - id: name_len
        type: u1
        doc: Length of the name string in the name section
      - id: fold_len
        type: u1
      - id: author_key_len
        type: u1
      - id: metadata_status
        type: u1
        enum: metadata_status
      - id: fold
        type: str
        encoding: UTF-8
        size: fold_len
      - id: fold_padding
        size: 96 - fold_len
      - id: author_key
        type: str
        encoding: UTF-8
        size: author_key_len
      - id: author_key_padding
        size: 12 - author_key_len
      - id: modification_time
        type: u4

    instances:
      name:
        io: _root.name_section._io
        pos: name_offset
        type: name_blob
  
      folder:
        value: _root.folders.entries[folder_id]

    types:
      name_blob:
        seq:
          - id: path_hash
            doc: FNV-1a fingerprint of the complete path
            type: u8
          - id: name
            type: str
            encoding: UTF-8
            size: _parent.name_len
          - id: canonical_author_len
            type: u1
          - id: canonical_author
            type: str
            size: canonical_author_len
            encoding: UTF-8
          - id: title_len
            type: u1
          - id: title
            type: str
            size: title_len
            encoding: UTF-8
          - id: author_len
            type: u1
          - id: author
            type: str
            size: author_len
            encoding: UTF-8
          - id: author_sort_len
            type: u1
          - id: author_sort
            doc: The book's primary-author file-as, empty if absent
            type: str
            size: author_sort_len
            encoding: UTF-8

  list_section:
    seq:
      - id: lists
        type: list_desc
        repeat: expr
        repeat-expr: _root.header.list_count

  list_desc:
    seq:
      - id: kind
        type: u1
        enum: list_kind
      - id: role
        type: u1
        enum: list_role
        doc: Built-in lists carry no label and are named by the firmware.
      - id: flags
        type: list_flags
      - id: label_len
        type: u1
      - id: entry_count
        type: u2
        doc: Rows in the list; book_count for an identity list.
      - id: reserved
        type: u2
      - id: entries_off
        type: u4
        doc: From list_start. Unused by an identity list.
      - id: label_off
        type: u4
        doc: From list_start.

    instances:
      entries:
        io: _parent._io
        pos: entries_off
        type: u2
        repeat: expr
        repeat-expr: entry_count
        if: kind != list_kind::identity
        doc: >
          Record ordinals for a books list; ids of LATER lists for a groups
          list, which is what keeps nesting free of cycles.
      label:
        io: _parent._io
        pos: label_off
        type: str
        size: label_len
        encoding: UTF-8
        if: label_len > 0

    types:
      list_flags:
        meta:
          bit-endian: le
        seq:
          - id: top_level
            type: b1
            doc: Shown as a tab or in the list picker.
          - id: reserved
            type: b7

  name_section_raw:
    seq:
      - id: raw_data
        size-eos: true

enums:
  metadata_status:
    0: not_attempted
    1: extracted
    2: failed
  list_kind:
    0: identity
    1: books
    2: groups
  list_role:
    0: external
    1: recent
    2: title
    3: author
