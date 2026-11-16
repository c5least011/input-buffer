-- run once on the already-created database (new installs use schema.sql)
ALTER TABLE configs ADD COLUMN tok TEXT NOT NULL DEFAULT '';
