#!/bin/sh
set -e -x

# Generate a set of certificates and keys for use by unit tests
# intended to be equivalent to gen_test_certs.py
# to check interoperability of openssl and java keytools created PKCS#12 files

die() {
    echo "$1" >&1
    exit 1
}

OUT="${1:-.}"
PW="${2:-changeit}"

[ "$OUT" = "-h" -o -d "$OUT" ] || die "usage: $0 [outdir] [password]"

rm -f \
    "$OUT"/ca-full.p12 "$OUT"/ca.pem "$OUT"/ca.p12 \
    "$OUT"/superserver1.p12 \
    "$OUT"/intermediateCA.p12 "$OUT"/intermediateCA.pem \
    "$OUT"/ioc1.p12 \
    "$OUT"/server1.p12 \
    "$OUT"/server2.p12 \
    "$OUT"/client1.p12 \
    "$OUT"/client2.p12

# the root CA private key is not needed during testing, so delete it on exit.
trap 'rm -f "$OUT"/ca-full.p12' EXIT QUIT TERM KILL

echo "==== Creating rootCA ===="

keytool -v -genkeypair -alias rootCA \
        -keystore "$OUT"/ca-full.p12 -storepass "$PW" \
        -dname "CN=rootCA" -keyalg RSA \
        -ext BasicConstraints=ca:true \
        -ext KeyUsage=cRLSign,keyCertSign
keytool -v -exportcert -alias rootCA \
        -keystore "$OUT"/ca-full.p12 -storepass "$PW" \
        -rfc -file "$OUT"/ca.pem
keytool -v -importcert -alias rootCA \
        -keystore "$OUT"/ca.p12 -storepass "$PW" \
        -file "$OUT"/ca.pem -noprompt

echo "==== Creating superserver1 ===="

keytool -v -genkeypair -alias superserver1 \
        -keystore "$OUT"/superserver1.p12 -storepass "$PW" \
        -dname "CN=dummy" \
        -keyalg RSA
keytool -v -importcert -alias rootCA \
        -keystore "$OUT"/superserver1.p12 -storepass "$PW" \
        -file "$OUT"/ca.pem \
        -noprompt
keytool -v -certreq -alias superserver1 \
        -keystore "$OUT"/superserver1.p12 -storepass "$PW" \
| keytool -v -gencert -alias rootCA \
        -keystore "$OUT"/ca-full.p12 -storepass "$PW" \
        -dname "CN=superserver1" \
        -ext KeyUsage=digitalSignature -ext ExtendedKeyUsage=serverAuth,clientAuth \
| keytool -v -importcert -alias superserver1 \
        -keystore "$OUT"/superserver1.p12 -storepass "$PW"

echo "==== Creating intermediateCA ===="

keytool -v -genkeypair -alias intermediateCA \
        -keystore "$OUT"/intermediateCA.p12 -storepass "$PW" \
        -dname "CN=dummy" \
        -keyalg RSA
keytool -v -importcert -alias rootCA \
        -keystore "$OUT"/intermediateCA.p12 -storepass "$PW" \
        -file "$OUT"/ca.pem \
        -noprompt
keytool -v -certreq -alias intermediateCA \
        -keystore "$OUT"/intermediateCA.p12 -storepass "$PW" \
| keytool -v -gencert -alias rootCA \
        -keystore "$OUT"/ca-full.p12 -storepass "$PW" \
        -dname "CN=intermediateCA" \
        -ext BasicConstraints=ca:true \
        -ext KeyUsage=digitalSignature,cRLSign,keyCertSign \
        -ext ExtendedKeyUsage=serverAuth,clientAuth,OCSPSigning \
        -outfile "$OUT"/intermediateCA.pem
keytool -v -importcert -alias intermediateCA \
        -keystore "$OUT"/intermediateCA.p12 -storepass "$PW" \
        -file "$OUT"/intermediateCA.pem

for name in ioc1 server1 server2 client1 client2
do
    echo "==== Creating $name ===="

    expr match "$name" server >/dev/null && EKU=serverAuth || true
    expr match "$name" client >/dev/null && EKU=clientAuth || true
    expr match "$name" ioc >/dev/null && EKU=clientAuth,serverAuth || true

    keytool -v -genkeypair -alias "$name" \
            -keystore "$OUT/$name.p12" -storepass "$PW" \
            -dname "CN=dummy" \
            -keyalg RSA
    keytool -v -importcert -alias rootCA \
            -keystore "$OUT/$name.p12" -storepass "$PW" \
            -file "$OUT"/ca.pem \
            -noprompt
    keytool -v -importcert -alias intermediateCA \
            -keystore "$OUT/$name.p12" -storepass "$PW" \
            -file "$OUT"/intermediateCA.pem \
            -noprompt
    keytool -v -certreq -alias "$name" \
            -keystore "$OUT/$name.p12" -storepass "$PW" \
    | keytool -v -gencert -alias intermediateCA \
            -keystore "$OUT"/intermediateCA.p12 -storepass "$PW" \
            -dname "CN=$name" \
            -ext KeyUsage=digitalSignature,keyEncipherment \
            -ext ExtendedKeyUsage="$EKU" \
    | keytool -v -importcert -alias "$name" \
            -keystore "$OUT/$name.p12" -storepass "$PW"

done

echo "==== Listing ===="

for ff in "$OUT"/*.p12
do
    echo "==== Listing $ff ===="
    keytool -v -list -keystore "$ff" -storepass "$PW"
done
