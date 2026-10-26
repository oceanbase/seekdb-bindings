pub fn open(path: &std::path::Path) -> Result<seekdb::Seekdb, seekdb::Error> {
    seekdb::Seekdb::open(path)
}
