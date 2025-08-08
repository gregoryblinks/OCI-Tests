#!/usr/bin/env python3
import oci
import subprocess
import sys

# === Step 1: Ask for compartment name ===
compartment_name = input("Enter the compartment name (e.g., OCI-LAB-##): ").strip()

# === Step 2: Load OCI config and clients ===
config = oci.config.from_file()
identity = oci.identity.IdentityClient(config)
resource_search_client = oci.resource_search.ResourceSearchClient(config)

# === Step 3: Get list of compartments (including tenancy root) ===
compartments = oci.pagination.list_call_get_all_results(
    identity.list_compartments,
    config["tenancy"],
    compartment_id_in_subtree=True
).data
compartments.append(identity.get_compartment(config["tenancy"]).data)

# Find compartment OCID
compartment_ocid = None
for c in compartments:
    if c.name == compartment_name and c.lifecycle_state == "ACTIVE":
        compartment_ocid = c.id
        break

if not compartment_ocid:
    print(f"❌ Compartment '{compartment_name}' not found or not active.")
    sys.exit(1)

print(f"🔎 Compartment OCID: {compartment_ocid}")

# === Step 4: Run ociLabMgmt.py (pre-clean) ===
oci_lab_cmd = ["./ociLabMgmt.py", "--delete", "--compartment", compartment_name]
print("\n🧹 Running ociLabMgmt cleanup:")
print(" ".join(oci_lab_cmd))
try:
    subprocess.run(oci_lab_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ ociLabMgmt.py failed: {e}")
    sys.exit(1)

# === Step 5: Define priority billable resources (used in 2nd pass) ===
important_billable_resources = {
    # Compute & Storage
    "Instance", "BootVolume", "Volume", "Image", "InstancePool",
    "VolumeBackup", "BootVolumeBackup", "VolumeGroup",
    # Database
    "DbSystem", "AutonomousDatabase", "AutonomousDatabaseBackup",
    # Load Balancer
    "LoadBalancer",
    # Object / File Storage
    "Bucket", "FileSystem", "MountTarget",
    # Streaming
    "Stream", "StreamPool",
    # Vault & Security
    "Vault", "Key", "Secret",
    # Containers
    "Cluster", "NodePool",
    # Analytics / Integration
    "AnalyticsInstance", "IntegrationInstance",
    # Serverless & APIs
    "Function", "ApiGateway", "ApiDeployment",
    # Monitoring / Logging
    "Alarm", "LogGroup", "Log",
    # Networking
    "Vcn", "Subnet", "Drg", "InternetGateway",
    "NATGateway", "ServiceGateway", "RouteTable", "SecurityList",
    # Access / Automation
    "ServiceConnector", "Bastion"
}

# Map billable types to *exact* OCI searchable names (silent if unsupported)
try:
    supported_resource_types = [t.name for t in resource_search_client.list_resource_types().data]
except Exception as e:
    print(f"❌ Failed to fetch supported resource types: {e}")
    sys.exit(1)

supported_lower = {t.lower(): t for t in supported_resource_types}
billable_types = [
    supported_lower[rtype.lower()]
    for rtype in sorted(important_billable_resources)
    if rtype.lower() in supported_lower
]

# === Step 6: Region scan (searchable-first, then billable) ===
regions = identity.list_region_subscriptions(config["tenancy"]).data
found_regions = set()
reason_by_region = {}  # region -> "searchable" or "billable"

def is_active(item):
    # Treat as active if lifecycle_state absent or not terminal
    st = getattr(item, "lifecycle_state", None)
    if not st:
        return True
    st_u = str(st).upper()
    return st_u not in {"TERMINATED", "DELETED", "INACTIVE"}

print("\n🌍 Checking regions (searchable-first, billable second)...")
for region in regions:
    region_name = region.region_name
    config["region"] = region_name
    resource_search_client = oci.resource_search.ResourceSearchClient(config)

    # --- PASS 1: Searchable-first (Tenancy Explorer parity) ---
    try:
        # No lifecycle filter in query; filter in Python to avoid parse errors
        result = resource_search_client.search_resources(
            search_details=oci.resource_search.models.StructuredSearchDetails(
                query=f"query all resources where compartmentId = '{compartment_ocid}'",
                type="Structured"
            ),
            limit=25
        ).data.items

        active = [it for it in result if is_active(it)]
        if active:
            print(f"✅ {region_name}: Found searchable resources:")
            for it in active[:10]:
                print(f"    • {it.resource_type} :: {it.display_name} (state={getattr(it,'lifecycle_state',None)})")
            found_regions.add(region_name)
            reason_by_region[region_name] = "searchable"
            continue  # don’t run billable pass if searchable already found
        else:
            print(f"ℹ️ {region_name}: Searchable query returned only terminal/empty results")

    except oci.exceptions.ServiceError as e:
        print(f"⚠️ Search error in {region_name}: {e.code} - {e.message}")
    except Exception as e:
        print(f"⚠️ Unexpected search error in {region_name}: {e}")

    # --- PASS 2: Billable types (only if PASS 1 found nothing) ---
    billable_hit = False
    for rtype in billable_types:
        q = f"query {rtype} resources where compartmentId = '{compartment_ocid}'"
        try:
            items = resource_search_client.search_resources(
                search_details=oci.resource_search.models.StructuredSearchDetails(
                    query=q,
                    type="Structured"
                ),
                limit=25
            ).data.items
            active_items = [it for it in items if is_active(it)]
            if active_items:
                if not billable_hit:
                    print(f"🟩 {region_name}: Found billable resources:")
                    billable_hit = True
                for it in active_items[:10]:
                    print(f"    • {it.resource_type} :: {it.display_name} (state={getattr(it,'lifecycle_state',None)})")
                # Keep looping a couple of types for visibility; remove if you prefer speed
        except oci.exceptions.ServiceError as e:
            print(f"   ⚠️ {region_name}:{rtype} search error: {e.code}")
        except Exception:
            pass

    if billable_hit:
        found_regions.add(region_name)
        reason_by_region[region_name] = "billable"
    else:
        print(f"❌ {region_name}: No resources found")

# === Step 7: Show summary and run cleanup ===
if not found_regions:
    print("\n🚫 No active regions with resources found — skipping cleanup.py.")
    sys.exit(0)

print("\n📋 Region summary:")
for r in sorted(found_regions):
    print(f" - {r} (from {reason_by_region.get(r)})")

region_list = ",".join(sorted(found_regions))
cleanup_cmd = ["./cleanup.py", "-c", compartment_name, "-r", region_list]

print("\n🧹 Running cleanup.py:")
print(" ".join(cleanup_cmd))
try:
    subprocess.run(cleanup_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ cleanup.py failed: {e}")
