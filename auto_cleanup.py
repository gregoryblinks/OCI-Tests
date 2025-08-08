#!/usr/bin/env python3
import oci
import subprocess

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
    exit(1)

print(f"🔎 Compartment OCID: {compartment_ocid}")

# === Step 4: Define priority billable resources ===
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

    # Automated connectors
    "ServiceConnector", "Bastion"
}

# === Step 5: Build searchable types lists ===
try:
    supported_resource_types = [t.name for t in resource_search_client.list_resource_types().data]
except Exception as e:
    print(f"❌ Failed to fetch supported resource types: {e}")
    exit(1)

supported_lower = {t.lower(): t for t in supported_resource_types}

# Priority list (keep only supported ones)
priority_types = []
for rtype in important_billable_resources:
    if rtype.lower() in supported_lower:
        priority_types.append(supported_lower[rtype.lower()])
    else:
        print(f"⚠️ Skipping unsupported resource type: {rtype}")

# Known non-billable types to skip in fallback
non_billable_keywords = {"compartment", "tag", "policy", "group", "user", "tenancy"}

# Fallback list = all supported types not in priority list, minus non-billable
fallback_types = [
    rt for rt in supported_resource_types
    if rt not in priority_types
    and not any(nb in rt.lower() for nb in non_billable_keywords)
]

# === Step 6: Get subscribed regions ===
regions = identity.list_region_subscriptions(config["tenancy"]).data
found_regions = set()

print("\n🌍 Checking regions for active resources with detailed debug output...")

for region in regions:
    config["region"] = region.region_name
    resource_search_client = oci.resource_search.ResourceSearchClient(config)
    found_in_region = False

    # ---- Priority search ----
    for rtype in priority_types:
        query = f"query {rtype} resources where compartmentId = '{compartment_ocid}' and lifecycleState != 'TERMINATED'"
        try:
            result = resource_search_client.search_resources(
                search_details=oci.resource_search.models.StructuredSearchDetails(
                    query=query,
                    type="Structured"
                ),
                limit=10
            ).data.items

            if result:
                print(f"✅ {region.region_name}: Found {rtype} resources:")
                for item in result:
                    print(f"    - {item.display_name} (State: {item.lifecycle_state})")
                found_in_region = True
                break  # no need to continue priority search for this region
        except Exception as e:
            print(f"⚠️ Error in {region.region_name} for {rtype}: {e}")

    # ---- Fallback search (if nothing found in priority) ----
    if not found_in_region:
        for rtype in fallback_types:
            query = f"query {rtype} resources where compartmentId = '{compartment_ocid}' and lifecycleState != 'TERMINATED'"
            try:
                result = resource_search_client.search_resources(
                    search_details=oci.resource_search.models.StructuredSearchDetails(
                        query=query,
                        type="Structured"
                    ),
                    limit=10
                ).data.items

                if result:
                    print(f"ℹ️ {region.region_name}: Found fallback {rtype} resources:")
                    for item in result:
                        print(f"    - {item.display_name} (State: {item.lifecycle_state})")
                    found_in_region = True
                    break
            except Exception:
                pass

    if found_in_region:
        found_regions.add(region.region_name)
    else:
        print(f"❌ No resources found in {region.region_name}")

# === Step 7: Run cleanup.py if needed ===
if not found_regions:
    print("🚫 No active regions with resources found — skipping cleanup.py.")
    exit(0)

region_list = ",".join(sorted(found_regions))
cleanup_cmd = ["./cleanup.py", "-c", compartment_name, "-r", region_list]

print("\n🧹 Running cleanup.py:")
print(" ".join(cleanup_cmd))

try:
    subprocess.run(cleanup_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ cleanup.py failed: {e}")
