import json
import boto3

dynamodb = boto3.resource('dynamodb')
table = dynamodb.Table('ee419_lab')

iot = boto3.client('iot-data')

CMD_TOPIC = 'ee419_lab/personstd-esp32s3/cmd'


def publish_assignment(nfc_tag, bit_position):

    payload = {
        'nfcTag': nfc_tag,
        'bitPosition': int(bit_position)
    }

    print(f"Publishing to {CMD_TOPIC}: {payload}")

    iot.publish(
        topic=CMD_TOPIC,
        qos=1,
        payload=json.dumps(payload)
    )

    print("Publish successful")


def lambda_handler(event, context):

    device_id = event['deviceId']

    print(f"Registration request received from {device_id}")

    # Check if device is already registered
    response = table.query(
        IndexName='DeviceId',
        KeyConditionExpression='deviceId = :d',
        ExpressionAttributeValues={
            ':d': device_id
        }
    )

    if response['Count'] > 0:

        existing = response['Items'][0]

        publish_assignment(
            existing['nfcTag'],
            existing['bitPosition']
        )

        return {
            'nfcTag': existing['nfcTag'],
            'bitPosition': int(existing['bitPosition']),
            'registered': True
        }

    # Find an unused NFC tag
    response = table.query(
        IndexName='DeviceId',
        KeyConditionExpression='deviceId = :n',
        ExpressionAttributeValues={
            ':n': 'None'
        }
    )

    if response['Count'] == 0:

        error_payload = {
            'error': 'All NFC tags already in use'
        }

        print("No available NFC tags")

        iot.publish(
            topic=CMD_TOPIC,
            qos=1,
            payload=json.dumps(error_payload)
        )

        return error_payload

    tag = response['Items'][0]

    # Assign NFC tag to the device
    table.update_item(
        Key={
            'nfcTag': tag['nfcTag']
        },
        UpdateExpression='SET deviceId = :d',
        ExpressionAttributeValues={
            ':d': device_id
        }
    )

    publish_assignment(
        tag['nfcTag'],
        tag['bitPosition']
    )

    return {
        'nfcTag': tag['nfcTag'],
        'bitPosition': int(tag['bitPosition']),
        'registered': False
    }
