#!/usr/bin/env python3
import oci
import subprocess

# Step 1: Ask for compartment name
compartment_name = input("Enter the compartment name (e.g., OCI-LAB-##): ").strip()

# Step 2: Load OCI config and get compartment OCID
config = oci.config.from_file()
identity = oci.identity.IdentityClient(config)
resource_search_client = oci.resource_search.ResourceSearchClient(config)

# Get list of compartments (including root)
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

# Step 3: Run ociLabMgmt.py
oci_lab_cmd = ["./ociLabMgmt.py", "--delete", "--compartment", compartment_name]
print("\n🧹 Running ociLabMgmt cleanup:")
print(" ".join(oci_lab_cmd))
try:
    subprocess.run(oci_lab_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ ociLabMgmt.py failed: {e}")
    exit(1)

# Step 4: Find regions with active billable resources
regions = identity.list_region_subscriptions(config["tenancy"]).data
found_regions = []

# Define refined billable + critical infrastructure resource types
important_billable_resources = {
    "Instance", "BootVolume", "Volume", "Image", "InstancePool",
    "VolumeBackup", "BootVolumeBackup", "VolumeGroup",
    "DbSystem", "AutonomousDatabase", "AutonomousDatabaseBackup",
    "LoadBalancer",
    "Bucket", "FileSystem", "MountTarget",
    "Stream", "StreamPool",
    "Vault", "Key", "Secret",
    "Cluster", "NodePool",
    "AnalyticsInstance", "IntegrationInstance",
    "Function", "ApiGateway", "ApiDeployment",
    "Alarm", "LogGroup", "Log",
    "Vcn", "Subnet", "Drg", "InternetGateway",
    "NATGateway", "ServiceGateway", "RouteTable", "SecurityList",
    "ServiceConnector", "Bastion"
}

# Fetch supported resource types from OCI and filter
try:
    supported_resource_types = [t.name for t in resource_search_client.list_resource_types().data]
except Exception as e:
    print(f"❌ Failed to fetch supported resource types: {e}")
    exit(1)

supported_lower = {t.lower(): t for t in supported_resource_types}

searchable_types = []
for rtype in important_billable_resources:
    rtype_lower = rtype.lower()
    if rtype_lower in supported_lower:
        searchable_types.append(supported_lower[rtype_lower])
    else:
        print(f"⚠️ Skipping unsupported resource type: {rtype}")

print("\n🌍 Checking regions for active resources...")

for region in regions:
    config["region"] = region.region_name
    resource_search_client = oci.resource_search.ResourceSearchClient(config)
    found_in_region = False

    # --- Primary check: billable searchable types ---
    for rtype in searchable_types:
        query = f"query {rtype} resources where compartmentId = '{compartment_ocid}' and lifecycleState != 'TERMINATED'"
        try:
            result = resource_search_client.search_resources(
                search_details=oci.resource_search.models.StructuredSearchDetails(
                    query=query,
                    type="Structured"
                ),
                limit=1
            ).data.items

            if result:
                print(f"✅ {region.region_name}: Found {rtype} ({result[0].display_name})")
                found_in_region = True
                break
        except oci.exceptions.ServiceError as e:
            print(f"⚠️ OCI Service Error in {region.region_name} for {rtype}: {e.code} - {e.message}")
        except Exception as e:
            print(f"⚠️ Unexpected error in {region.region_name} for {rtype}: {e}")

    # --- Fallback check: query all resources ---
    if not found_in_region:
        try:
            result = resource_search_client.search_resources(
                search_details=oci.resource_search.models.StructuredSearchDetails(
                    query=f"query all resources where compartmentId = '{compartment_ocid}'",
                    type="Structured"
                ),
                limit=1
            ).data.items

            if result:
                print(f"🔄 {region.region_name}: Found non-priority resource ({result[0].resource_type})")
                found_in_region = True
        except Exception as e:
            print(f"⚠️ Fallback error in {region.region_name}: {e}")

    if found_in_region:
        found_regions.append(region.region_name)
    else:
        print(f"❌ No resources found in {region.region_name}")

# Step 5: Run cleanup.py with region list
if not found_regions:
    print("🚫 No active regions with resources found — skipping cleanup.py.")
    exit(0)

region_list = ",".join(found_regions)
cleanup_cmd = ["./cleanup.py", "-c", compartment_name, "-r", region_list]

print("\n🧹 Running cleanup.py:")
print(" ".join(cleanup_cmd))

try:
    subprocess.run(cleanup_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ cleanup.py failed: {e}")
