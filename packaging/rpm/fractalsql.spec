%global         plugindir %{_libdir}/mysql/plugin
%global         sharedir  %{_datadir}/mariadb-fractalsql

# Caller passes --define "mdb_series 106|1011|114" to pick the matching
# prebuilt .so from dist/${arch}/fractal_mdb${mdb_series}.so, and
# --define "mdb_version_dotted 10.6|10.11|11.4" for the package name.
%{!?mdb_series:%global mdb_series 1011}
%{!?mdb_version_dotted:%global mdb_version_dotted 10.11}

Name:           mariadb-fractalsql-v%{mdb_version_dotted}
Version:        1.0.0
Release:        1%{?dist}
Summary:        Stochastic Fractal Search UDF for MariaDB %{mdb_version_dotted}

License:        MIT
URL:            https://github.com/FractalSQLabs/mariadb-fractalsql
Source0:        mariadb-fractalsql-%{version}.tar.gz

BuildRequires:  gcc, make, luajit-devel, MariaDB-devel
Requires:       luajit
Requires:       MariaDB-server >= %{mdb_version_dotted}

%description
mariadb-fractalsql registers the fractal_search() UDF, a LuaJIT-backed
Stochastic Fractal Search optimizer that returns JSON top-k matches
for a query vector against an inline corpus. Compiled against the
MariaDB %{mdb_version_dotted} UDF ABI.

%prep
%setup -q

%build
# The per-version .so is produced out-of-band by build.sh on a Docker
# builder; this spec just stages it into the RPM.
test -f dist/fractal_mdb%{mdb_series}.so

%install
install -Dm0755 dist/fractal_mdb%{mdb_series}.so \
    %{buildroot}%{plugindir}/fractalsql.so
install -Dm0644 sql/install_udf.sql \
    %{buildroot}%{sharedir}/install_udf.sql

%files
%license LICENSE
%{plugindir}/fractalsql.so
%{sharedir}/install_udf.sql

%changelog
* Sat Apr 18 2026 FractalSQLabs <ops@fractalsqlabs.io> - 1.0.0-1
- Initial Factory-standardized release for MariaDB 10.6 / 10.11 / 11.4.
